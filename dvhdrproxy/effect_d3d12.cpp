// effect_d3d12.cpp — D3D12 realisation of the DVHDR six-pass tonemap. The pass
// graph matches the D3D11 path, but D3D12 demands explicit plumbing: a root
// signature shared by the compute and graphics passes, PSOs built from the same
// SM 5.0 bytecode, a shader-visible CBV/SRV/UAV heap laid out so t0..t4 and
// u0..u1 form two contiguous descriptor tables, an RTV heap, and hand-managed
// resource-state transitions. We do not own a queue — the game's DIRECT queue
// is captured from its ExecuteCommandLists and our command list is submitted on
// it just before Present, preserving order.

#include "effect_d3d12.h"
#include "config.h"
#include <wrl/client.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

#include "dvhdr_dwm_vs_post.h"
#include "dvhdr_dwm_ps_blurh.h"
#include "dvhdr_dwm_ps_blurv.h"
#include "dvhdr_dwm_ps_tonemap.h"
#include "dvhdr_dwm_cs_clear.h"
#include "dvhdr_dwm_cs_analyze.h"
#include "dvhdr_dwm_cs_adapt.h"

static const D3D12_RESOURCE_STATES SR_READ =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// A resource paired with its tracked current state.
struct Res
{
    ComPtr<ID3D12Resource> r;
    D3D12_RESOURCE_STATES  state = D3D12_RESOURCE_STATE_COMMON;
};

static ComPtr<ID3D12Device>        g_device;
static ComPtr<ID3D12CommandQueue>  g_queue;          // captured from the game
static ComPtr<ID3D12RootSignature> g_rootSig;
static ComPtr<ID3D12PipelineState> g_psoClear, g_psoAnalyze, g_psoAdapt;
static ComPtr<ID3D12PipelineState> g_psoBlurH, g_psoBlurV;
static ComPtr<ID3D12PipelineState> g_psoTonemap;
static DXGI_FORMAT                 g_tonemapFmt = DXGI_FORMAT_UNKNOWN;

static ComPtr<ID3D12DescriptorHeap> g_srvHeap;       // shader-visible, 7 slots
static ComPtr<ID3D12DescriptorHeap> g_rtvHeap;       // 2 + bufferCount slots
static UINT g_srvInc = 0, g_rtvInc = 0;

static ComPtr<ID3D12CommandAllocator>    g_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_list;
static ComPtr<ID3D12Fence>               g_fence;
static UINT64 g_fenceValue = 0;
static HANDLE g_fenceEvent = NULL;

static ComPtr<ID3D12Resource> g_cbUpload;            // 256B upload, persistently mapped
static void*                  g_cbMapped = NULL;

static Res g_hist, g_adapt, g_scene, g_lumaH, g_lumaBlur;
static UINT g_sceneW = 0, g_sceneH = 0;
static DXGI_FORMAT g_sceneFmt = DXGI_FORMAT_UNKNOWN;
static UINT g_bufferCount = 0;

static LARGE_INTEGER g_qpcFreq = {};
static LARGE_INTEGER g_qpcLast = {};

// SRV/UAV heap slot layout (contiguous so each table is one range).
enum { SLOT_SCENE = 0, SLOT_HIST, SLOT_ADAPT, SLOT_LUMAH, SLOT_LUMABLUR, SLOT_HISTUAV, SLOT_ADAPTUAV, SLOT_COUNT };

void Effect12_SetQueue(ID3D12CommandQueue* queue)
{
    if (!queue) return;
    D3D12_COMMAND_QUEUE_DESC d = queue->GetDesc();
    if (d.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return; // present queue is DIRECT
    if (g_queue.Get() == queue) return;
    g_queue = queue;
}

static void WaitGpuIdle()
{
    if (!g_queue || !g_fence) return;
    UINT64 v = ++g_fenceValue;
    if (FAILED(g_queue->Signal(g_fence.Get(), v))) return;
    if (g_fence->GetCompletedValue() < v)
    {
        g_fence->SetEventOnCompletion(v, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 2000);
    }
}

static void TeardownSizeBound()
{
    g_scene = Res(); g_lumaH = Res(); g_lumaBlur = Res();
    g_sceneW = g_sceneH = 0; g_sceneFmt = DXGI_FORMAT_UNKNOWN;
    g_rtvHeap.Reset(); g_bufferCount = 0;
    g_psoTonemap.Reset(); g_tonemapFmt = DXGI_FORMAT_UNKNOWN;
}

static void TeardownDevice()
{
    if (g_queue && g_fence) WaitGpuIdle();
    TeardownSizeBound();
    g_hist = Res(); g_adapt = Res();
    if (g_cbUpload && g_cbMapped) { g_cbUpload->Unmap(0, NULL); g_cbMapped = NULL; }
    g_cbUpload.Reset();
    g_srvHeap.Reset();
    g_psoClear.Reset(); g_psoAnalyze.Reset(); g_psoAdapt.Reset();
    g_psoBlurH.Reset(); g_psoBlurV.Reset();
    g_rootSig.Reset();
    g_list.Reset(); g_alloc.Reset();
    g_fence.Reset();
    g_device.Reset();
}

void Effect12_Shutdown()
{
    TeardownDevice();
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = NULL; }
    g_queue.Reset();
}

void Effect12_OnResize()
{
    if (g_queue && g_fence) WaitGpuIdle();
    TeardownSizeBound();
}

static void Transition(Res& res, D3D12_RESOURCE_STATES to)
{
    if (res.state == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res.r.Get();
    b.Transition.StateBefore = res.state;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(1, &b);
    res.state = to;
}

static void TransitionRaw(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(1, &b);
}

static void UavBarrier(ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    g_list->ResourceBarrier(1, &b);
}

static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE t)
{
    D3D12_HEAP_PROPERTIES h = {};
    h.Type = t;
    return h;
}

static bool CreateTex(Res& out, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
                      D3D12_RESOURCE_STATES initState)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    auto hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initState,
                                                 NULL, IID_PPV_ARGS(&out.r)))) return false;
    out.state = initState;
    return true;
}

static D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(UINT slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * g_srvInc;
    return h;
}
static D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(UINT slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)slot * g_srvInc;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE RtvCpu(UINT slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * g_rtvInc;
    return h;
}

static bool BuildRootSig()
{
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 5;       // t0..t4
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 2;       // u0..u1
    uavRange.BaseShaderRegister = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;       // b0
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uavRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs = {};
    rs.NumParameters = 3;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;
    rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
    return SUCCEEDED(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&g_rootSig)));
}

static bool MakeCompute(ComPtr<ID3D12PipelineState>& pso, const void* bc, size_t n)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = g_rootSig.Get();
    d.CS.pShaderBytecode = bc;
    d.CS.BytecodeLength  = n;
    return SUCCEEDED(g_device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)));
}

static bool MakeGraphics(ComPtr<ID3D12PipelineState>& pso, const void* ps, size_t psN, DXGI_FORMAT rtv)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = g_rootSig.Get();
    d.VS.pShaderBytecode = g_VS_Post; d.VS.BytecodeLength = sizeof(g_VS_Post);
    d.PS.pShaderBytecode = ps;        d.PS.BytecodeLength = psN;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = 1;
    return SUCCEEDED(g_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)));
}

static bool EnsureTonemapPso(DXGI_FORMAT fmt)
{
    if (g_psoTonemap && g_tonemapFmt == fmt) return true;
    g_psoTonemap.Reset();
    if (!MakeGraphics(g_psoTonemap, g_PS_Tonemap, sizeof(g_PS_Tonemap), fmt)) return false;
    g_tonemapFmt = fmt;
    return true;
}

static bool EnsureDeviceObjects(ID3D12Device* dev)
{
    if (g_device.Get() == dev) return true;
    TeardownDevice();
    g_device = dev;

    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc)))) return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc.Get(), NULL, IID_PPV_ARGS(&g_list)))) return false;
    g_list->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fenceValue = 0;
    if (!g_fenceEvent) g_fenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);

    if (!BuildRootSig()) return false;
    if (!MakeCompute(g_psoClear,   g_CS_Clear,   sizeof(g_CS_Clear)))   return false;
    if (!MakeCompute(g_psoAnalyze, g_CS_Analyze, sizeof(g_CS_Analyze))) return false;
    if (!MakeCompute(g_psoAdapt,   g_CS_Adapt,   sizeof(g_CS_Adapt)))   return false;
    if (!MakeGraphics(g_psoBlurH, g_PS_BlurH, sizeof(g_PS_BlurH), DXGI_FORMAT_R16_FLOAT)) return false;
    if (!MakeGraphics(g_psoBlurV, g_PS_BlurV, sizeof(g_PS_BlurV), DXGI_FORMAT_R16_FLOAT)) return false;

    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = SLOT_COUNT;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_srvHeap)))) return false;
        g_srvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // Persistent analysis state (histogram + temporal adapt), kept in UAV state.
    if (!CreateTex(g_hist,  256, 1, DXGI_FORMAT_R32_SINT,          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) return false;
    if (!CreateTex(g_adapt, 1,   1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) return false;

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R32_SINT;
        g_device->CreateShaderResourceView(g_hist.r.Get(), &sd, SrvCpu(SLOT_HIST));
        sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        g_device->CreateShaderResourceView(g_adapt.r.Get(), &sd, SrvCpu(SLOT_ADAPT));

        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        ud.Format = DXGI_FORMAT_R32_SINT;
        g_device->CreateUnorderedAccessView(g_hist.r.Get(), NULL, &ud, SrvCpu(SLOT_HISTUAV));
        ud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        g_device->CreateUnorderedAccessView(g_adapt.r.Get(), NULL, &ud, SrvCpu(SLOT_ADAPTUAV));
    }

    {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                   D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&g_cbUpload)))) return false;
        D3D12_RANGE none = { 0, 0 };
        if (FAILED(g_cbUpload->Map(0, &none, &g_cbMapped))) return false;
    }

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcLast);
    return true;
}

static bool EnsureSizeBound(IDXGISwapChain* swap, UINT w, UINT h, DXGI_FORMAT fmt)
{
    DXGI_SWAP_CHAIN_DESC scd;
    if (FAILED(swap->GetDesc(&scd))) return false;

    bool sizeOk = g_scene.r && fmt == g_sceneFmt && w == g_sceneW && h == g_sceneH;
    bool heapOk = g_rtvHeap && g_bufferCount == scd.BufferCount;
    if (sizeOk && heapOk) return true;

    if (g_queue && g_fence) WaitGpuIdle();
    g_scene = Res(); g_lumaH = Res(); g_lumaBlur = Res();

    if (!CreateTex(g_scene, w, h, fmt, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST)) return false;
    if (!CreateTex(g_lumaH, w, h, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) return false;
    if (!CreateTex(g_lumaBlur, w, h, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) return false;

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = fmt;
        g_device->CreateShaderResourceView(g_scene.r.Get(), &sd, SrvCpu(SLOT_SCENE));
        sd.Format = DXGI_FORMAT_R16_FLOAT;
        g_device->CreateShaderResourceView(g_lumaH.r.Get(), &sd, SrvCpu(SLOT_LUMAH));
        g_device->CreateShaderResourceView(g_lumaBlur.r.Get(), &sd, SrvCpu(SLOT_LUMABLUR));
    }

    if (!heapOk)
    {
        g_rtvHeap.Reset();
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2 + scd.BufferCount;   // lumaH, lumaBlur, then back buffers
        if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap)))) return false;
        g_rtvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g_bufferCount = scd.BufferCount;
    }
    g_device->CreateRenderTargetView(g_lumaH.r.Get(), NULL, RtvCpu(0));
    g_device->CreateRenderTargetView(g_lumaBlur.r.Get(), NULL, RtvCpu(1));

    g_sceneW = w; g_sceneH = h; g_sceneFmt = fmt;
    return true;
}

static void UpdateCbuffer(UINT w, UINT h, UINT colorSpace)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = (double)(now.QuadPart - g_qpcLast.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
    g_qpcLast = now;
    if (dt > 1000.0 || dt <= 0.0) dt = 16.6;

    DvhdrCbGpu cb = {};
    Config_FillCbuffer(&cb, w, h, colorSpace, (float)dt);
    memcpy(g_cbMapped, &cb, sizeof(cb));
}

bool Effect12_Apply(IDXGISwapChain* swap)
{
    if (!g_queue) return false; // no queue captured yet — present unmodified

    ComPtr<ID3D12Device> dev;
    if (FAILED(swap->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return false; // not D3D12

    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(swap->QueryInterface(IID_PPV_ARGS(&sc3)))) return false;

    UINT idx = sc3->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> bb;
    if (FAILED(swap->GetBuffer(idx, IID_PPV_ARGS(&bb))) || !bb) return false;

    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    UINT colorSpace = Config_ColorSpaceForFormat(bd.Format);
    if (colorSpace == 0) return false;

    if (!EnsureDeviceObjects(dev.Get())) return false;
    if (!EnsureSizeBound(swap, (UINT)bd.Width, bd.Height, bd.Format)) return false;
    if (!EnsureTonemapPso(bd.Format)) return false;

    // Reclaim last frame's allocator before reusing it.
    if (g_fenceValue != 0)
    {
        if (g_fence->GetCompletedValue() < g_fenceValue)
        {
            g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 2000);
        }
    }
    if (FAILED(g_alloc->Reset())) return false;
    if (FAILED(g_list->Reset(g_alloc.Get(), NULL))) return false;

    UpdateCbuffer((UINT)bd.Width, bd.Height, colorSpace);

    // Back buffer arrives in PRESENT; copy it into the scene texture.
    TransitionRaw(bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(g_scene, D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->CopyResource(g_scene.r.Get(), bb.Get());
    Transition(g_scene, SR_READ);

    ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
    g_list->SetDescriptorHeaps(1, heaps);

    // ---- compute: histogram clear / accumulate / adapt ----
    g_list->SetComputeRootSignature(g_rootSig.Get());
    g_list->SetComputeRootConstantBufferView(0, g_cbUpload->GetGPUVirtualAddress());
    g_list->SetComputeRootDescriptorTable(1, SrvGpu(SLOT_SCENE));
    g_list->SetComputeRootDescriptorTable(2, SrvGpu(SLOT_HISTUAV));

    g_list->SetPipelineState(g_psoClear.Get());
    g_list->Dispatch(1, 1, 1);
    UavBarrier(g_hist.r.Get()); UavBarrier(g_adapt.r.Get());

    g_list->SetPipelineState(g_psoAnalyze.Get());
    UINT stride = (g_knobs.AnalyzeStride >= 1) ? (UINT)g_knobs.AnalyzeStride : 1u;
    UINT step = stride * 16u;
    g_list->Dispatch(((UINT)bd.Width + step - 1u) / step, (bd.Height + step - 1u) / step, 1);
    UavBarrier(g_hist.r.Get());

    g_list->SetPipelineState(g_psoAdapt.Get());
    g_list->Dispatch(1, 1, 1);
    UavBarrier(g_hist.r.Get()); UavBarrier(g_adapt.r.Get());

    // hist + adapt are read as SRVs in the tonemap pass.
    Transition(g_hist, SR_READ); Transition(g_adapt, SR_READ);

    // ---- graphics: separable blur then tonemap ----
    g_list->SetGraphicsRootSignature(g_rootSig.Get());
    g_list->SetGraphicsRootConstantBufferView(0, g_cbUpload->GetGPUVirtualAddress());
    g_list->SetGraphicsRootDescriptorTable(1, SrvGpu(SLOT_SCENE));
    g_list->SetGraphicsRootDescriptorTable(2, SrvGpu(SLOT_HISTUAV));
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VIEWPORT vp = { 0.f, 0.f, (float)bd.Width, (float)bd.Height, 0.f, 1.f };
    D3D12_RECT     rc = { 0, 0, (LONG)bd.Width, (LONG)bd.Height };
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &rc);

    Transition(g_lumaH, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = RtvCpu(0);
    g_list->OMSetRenderTargets(1, &rtvH, FALSE, NULL);
    g_list->SetPipelineState(g_psoBlurH.Get());
    g_list->DrawInstanced(3, 1, 0, 0);

    Transition(g_lumaH, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(g_lumaBlur, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvB = RtvCpu(1);
    g_list->OMSetRenderTargets(1, &rtvB, FALSE, NULL);
    g_list->SetPipelineState(g_psoBlurV.Get());
    g_list->DrawInstanced(3, 1, 0, 0);

    Transition(g_lumaBlur, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    TransitionRaw(bb.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    UINT bbSlot = 2 + idx;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvBB = RtvCpu(bbSlot);
    g_device->CreateRenderTargetView(bb.Get(), NULL, rtvBB);
    g_list->OMSetRenderTargets(1, &rtvBB, FALSE, NULL);
    g_list->SetPipelineState(g_psoTonemap.Get());
    g_list->DrawInstanced(3, 1, 0, 0);

    // Restore states for next frame / present.
    TransitionRaw(bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    Transition(g_hist, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(g_adapt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(g_scene, D3D12_RESOURCE_STATE_COPY_DEST);

    if (FAILED(g_list->Close())) return false;
    ID3D12CommandList* lists[] = { g_list.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence.Get(), ++g_fenceValue);
    return true;
}
