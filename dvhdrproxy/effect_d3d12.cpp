// effect_d3d12.cpp - D3D12 realisation of the DVHDR six-pass tonemap. The pass
// graph matches the D3D11 path, but D3D12 demands explicit plumbing: a root
// signature shared by the compute and graphics passes, PSOs built from the same
// SM 5.0 bytecode, a shader-visible CBV/SRV/UAV heap laid out so t0..t4 and
// u0..u1 form two contiguous descriptor tables, an RTV heap, and hand-managed
// resource-state transitions. We do not own a queue - the game's DIRECT queue
// is captured from its ExecuteCommandLists (one per device) and our command
// list is submitted on it just before Present, preserving order.
//
// As in the D3D11 path, state lives per swap chain so several chains on several
// devices never share (and never tear down) each other's objects. The caller
// (hook.cpp) serialises Apply and OnResize; the queue table has its own lock
// because ExecuteCommandLists arrives from any thread.

#include "effect_d3d12.h"
#include "config.h"
#include "log.h"
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

// SRV/UAV heap slot layout (contiguous so each table is one range).
enum { SLOT_SCENE = 0, SLOT_HIST, SLOT_ADAPT, SLOT_LUMAH, SLOT_LUMABLUR, SLOT_HISTUAV, SLOT_ADAPTUAV, SLOT_COUNT };

struct Fx12
{
    IDXGISwapChain* key     = NULL;   // identity only: never dereferenced, never retained
    ULONGLONG       lastUse = 0;

    ComPtr<ID3D12Device>        device;
    ComPtr<ID3D12CommandQueue>  queue;          // the queue our last submission went to
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> psoClear, psoAnalyze, psoAdapt;
    ComPtr<ID3D12PipelineState> psoBlurH, psoBlurV;
    ComPtr<ID3D12PipelineState> psoTonemap;
    DXGI_FORMAT                 tonemapFmt = DXGI_FORMAT_UNKNOWN;

    ComPtr<ID3D12DescriptorHeap> srvHeap;       // shader-visible, SLOT_COUNT slots
    ComPtr<ID3D12DescriptorHeap> rtvHeap;       // 2 + bufferCount slots
    UINT srvInc = 0, rtvInc = 0;

    ComPtr<ID3D12CommandAllocator>    alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence>               fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = NULL;

    ComPtr<ID3D12Resource> cbUpload;            // 256B upload, persistently mapped
    void*                  cbMapped = NULL;

    Res hist, adapt, scene, lumaH, lumaBlur;
    UINT sceneW = 0, sceneH = 0;
    DXGI_FORMAT sceneFmt = DXGI_FORMAT_UNKNOWN;
    bool sceneValid = false;   // holds a complete untonemapped frame
    UINT bufferCount = 0;

    LARGE_INTEGER qpcFreq = {};
    LARGE_INTEGER qpcLast = {};
};

static const int kMaxSlots = 4;
static Fx12 g_slots[kMaxSlots];

// ---- captured present queues, one per device ------------------------------

struct QueueEntry
{
    ComPtr<ID3D12Device>       device;
    ComPtr<ID3D12CommandQueue> queue;
};
static QueueEntry g_queues[kMaxSlots];
static SRWLOCK    g_queueLock = SRWLOCK_INIT;

void Effect12_SetQueue(ID3D12CommandQueue* queue)
{
    if (!queue) return;

    AcquireSRWLockShared(&g_queueLock);
    bool known = false;
    for (int i = 0; i < kMaxSlots && !known; i++) known = (g_queues[i].queue.Get() == queue);
    ReleaseSRWLockShared(&g_queueLock);
    if (known) return;

    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return; // present queue is DIRECT
    ComPtr<ID3D12Device> dev;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return;

    AcquireSRWLockExclusive(&g_queueLock);
    int slot = -1;
    for (int i = 0; i < kMaxSlots && slot < 0; i++)
        if (g_queues[i].device.Get() == dev.Get()) slot = i;
    for (int i = 0; i < kMaxSlots && slot < 0; i++)
        if (!g_queues[i].device) slot = i;
    if (slot < 0) slot = 0;
    g_queues[slot].device = dev;
    g_queues[slot].queue  = queue;
    ReleaseSRWLockExclusive(&g_queueLock);
}

static ComPtr<ID3D12CommandQueue> QueueForDevice(ID3D12Device* dev)
{
    ComPtr<ID3D12CommandQueue> q;
    AcquireSRWLockShared(&g_queueLock);
    for (int i = 0; i < kMaxSlots; i++)
        if (g_queues[i].device.Get() == dev) { q = g_queues[i].queue; break; }
    ReleaseSRWLockShared(&g_queueLock);
    return q;
}

// ---- slot lifetime ----------------------------------------------------------

static void WaitGpuIdle(Fx12& s)
{
    if (!s.queue || !s.fence) return;
    UINT64 v = ++s.fenceValue;
    if (FAILED(s.queue->Signal(s.fence.Get(), v))) return;
    if (s.fence->GetCompletedValue() < v)
    {
        s.fence->SetEventOnCompletion(v, s.fenceEvent);
        WaitForSingleObject(s.fenceEvent, 2000);
    }
}

static void TeardownSizeBound(Fx12& s)
{
    s.scene = Res(); s.lumaH = Res(); s.lumaBlur = Res();
    s.sceneW = s.sceneH = 0; s.sceneFmt = DXGI_FORMAT_UNKNOWN; s.sceneValid = false;
    s.rtvHeap.Reset(); s.bufferCount = 0;
    s.psoTonemap.Reset(); s.tonemapFmt = DXGI_FORMAT_UNKNOWN;
}

static void TeardownDevice(Fx12& s)
{
    WaitGpuIdle(s);
    TeardownSizeBound(s);
    s.hist = Res(); s.adapt = Res();
    if (s.cbUpload && s.cbMapped) { s.cbUpload->Unmap(0, NULL); s.cbMapped = NULL; }
    s.cbUpload.Reset();
    s.srvHeap.Reset();
    s.psoClear.Reset(); s.psoAnalyze.Reset(); s.psoAdapt.Reset();
    s.psoBlurH.Reset(); s.psoBlurV.Reset();
    s.rootSig.Reset();
    s.list.Reset(); s.alloc.Reset();
    s.fence.Reset(); s.fenceValue = 0;
    s.queue.Reset();
    s.device.Reset();
}

static void ReleaseSlot(Fx12& s)
{
    TeardownDevice(s);
    if (s.fenceEvent) { CloseHandle(s.fenceEvent); s.fenceEvent = NULL; }
    s.key = NULL;
    s.lastUse = 0;
}

static Fx12* FindSlot(IDXGISwapChain* key)
{
    for (int i = 0; i < kMaxSlots; i++)
        if (g_slots[i].key == key) return &g_slots[i];
    return NULL;
}

static Fx12& AcquireSlot(IDXGISwapChain* key)
{
    Fx12* s = FindSlot(key);
    if (s) return *s;

    Fx12* victim = &g_slots[0];
    for (int i = 0; i < kMaxSlots; i++)
    {
        if (!g_slots[i].key) { victim = &g_slots[i]; break; }
        if (g_slots[i].lastUse < victim->lastUse) victim = &g_slots[i];
    }
    ReleaseSlot(*victim);
    victim->key = key;
    return *victim;
}

void Effect12_Shutdown()
{
    for (int i = 0; i < kMaxSlots; i++) ReleaseSlot(g_slots[i]);
    AcquireSRWLockExclusive(&g_queueLock);
    for (int i = 0; i < kMaxSlots; i++) { g_queues[i].queue.Reset(); g_queues[i].device.Reset(); }
    ReleaseSRWLockExclusive(&g_queueLock);
}

void Effect12_OnResize(IDXGISwapChain* swap)
{
    Fx12* s = FindSlot(swap);
    if (!s) return;
    WaitGpuIdle(*s);
    TeardownSizeBound(*s);
}

// ---- helpers ----------------------------------------------------------------

static void Transition(Fx12& s, Res& res, D3D12_RESOURCE_STATES to)
{
    if (res.state == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res.r.Get();
    b.Transition.StateBefore = res.state;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s.list->ResourceBarrier(1, &b);
    res.state = to;
}

static void TransitionRaw(Fx12& s, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s.list->ResourceBarrier(1, &b);
}

static void UavBarrier(Fx12& s, ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    s.list->ResourceBarrier(1, &b);
}

static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE t)
{
    D3D12_HEAP_PROPERTIES h = {};
    h.Type = t;
    return h;
}

static bool CreateTex(Fx12& s, Res& out, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
                      D3D12_RESOURCE_STATES initState)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    auto hp = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(s.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, initState,
                                                 NULL, IID_PPV_ARGS(&out.r)))) return false;
    out.state = initState;
    return true;
}

static D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(Fx12& s, UINT slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = s.srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * s.srvInc;
    return h;
}
static D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(Fx12& s, UINT slot)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = s.srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)slot * s.srvInc;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE RtvCpu(Fx12& s, UINT slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = s.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)slot * s.rtvInc;
    return h;
}

static bool BuildRootSig(Fx12& s)
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
    return SUCCEEDED(s.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   IID_PPV_ARGS(&s.rootSig)));
}

static bool MakeCompute(Fx12& s, ComPtr<ID3D12PipelineState>& pso, const void* bc, size_t n)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = s.rootSig.Get();
    d.CS.pShaderBytecode = bc;
    d.CS.BytecodeLength  = n;
    return SUCCEEDED(s.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)));
}

static bool MakeGraphics(Fx12& s, ComPtr<ID3D12PipelineState>& pso, const void* ps, size_t psN, DXGI_FORMAT rtv)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = s.rootSig.Get();
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
    return SUCCEEDED(s.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)));
}

static bool EnsureTonemapPso(Fx12& s, DXGI_FORMAT fmt)
{
    if (s.psoTonemap && s.tonemapFmt == fmt) return true;
    s.psoTonemap.Reset();
    if (!MakeGraphics(s, s.psoTonemap, g_PS_Tonemap, sizeof(g_PS_Tonemap), fmt)) return false;
    s.tonemapFmt = fmt;
    return true;
}

static bool CreateDeviceObjects(Fx12& s, ID3D12Device* dev)
{
    s.device = dev;

    if (FAILED(s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc)))) return false;
    if (FAILED(s.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc.Get(), NULL, IID_PPV_ARGS(&s.list)))) return false;
    s.list->Close();
    if (FAILED(s.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence)))) return false;
    s.fenceValue = 0;
    if (!s.fenceEvent) s.fenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s.fenceEvent) return false;

    if (!BuildRootSig(s)) return false;
    if (!MakeCompute(s, s.psoClear,   g_CS_Clear,   sizeof(g_CS_Clear)))   return false;
    if (!MakeCompute(s, s.psoAnalyze, g_CS_Analyze, sizeof(g_CS_Analyze))) return false;
    if (!MakeCompute(s, s.psoAdapt,   g_CS_Adapt,   sizeof(g_CS_Adapt)))   return false;
    if (!MakeGraphics(s, s.psoBlurH, g_PS_BlurH, sizeof(g_PS_BlurH), DXGI_FORMAT_R16_FLOAT)) return false;
    if (!MakeGraphics(s, s.psoBlurV, g_PS_BlurV, sizeof(g_PS_BlurV), DXGI_FORMAT_R16_FLOAT)) return false;

    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = SLOT_COUNT;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(s.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.srvHeap)))) return false;
        s.srvInc = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // Persistent analysis state (histogram + temporal adapt), kept in UAV state.
    if (!CreateTex(s, s.hist,  256, 1, DXGI_FORMAT_R32_SINT,          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) return false;
    if (!CreateTex(s, s.adapt, 1,   1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) return false;

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R32_SINT;
        s.device->CreateShaderResourceView(s.hist.r.Get(), &sd, SrvCpu(s, SLOT_HIST));
        sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        s.device->CreateShaderResourceView(s.adapt.r.Get(), &sd, SrvCpu(s, SLOT_ADAPT));

        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        ud.Format = DXGI_FORMAT_R32_SINT;
        s.device->CreateUnorderedAccessView(s.hist.r.Get(), NULL, &ud, SrvCpu(s, SLOT_HISTUAV));
        ud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        s.device->CreateUnorderedAccessView(s.adapt.r.Get(), NULL, &ud, SrvCpu(s, SLOT_ADAPTUAV));
    }

    {
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto hp = HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        if (FAILED(s.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                   D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&s.cbUpload)))) return false;
        D3D12_RANGE none = { 0, 0 };
        if (FAILED(s.cbUpload->Map(0, &none, &s.cbMapped))) return false;
    }

    QueryPerformanceFrequency(&s.qpcFreq);
    QueryPerformanceCounter(&s.qpcLast);
    return true;
}

// A half-built slot is torn down again rather than left with holes for the
// next frame to fall through.
static bool EnsureDeviceObjects(Fx12& s, ID3D12Device* dev)
{
    if (s.device.Get() == dev) return true;
    TeardownDevice(s);
    if (!CreateDeviceObjects(s, dev))
    {
        TeardownDevice(s);
        return false;
    }
    return true;
}

static bool EnsureSizeBound(Fx12& s, IDXGISwapChain* swap, UINT w, UINT h, DXGI_FORMAT fmt)
{
    DXGI_SWAP_CHAIN_DESC scd;
    if (FAILED(swap->GetDesc(&scd))) return false;

    bool sizeOk = s.scene.r && fmt == s.sceneFmt && w == s.sceneW && h == s.sceneH;
    bool heapOk = s.rtvHeap && s.bufferCount == scd.BufferCount;
    if (sizeOk && heapOk) return true;

    WaitGpuIdle(s);
    s.scene = Res(); s.lumaH = Res(); s.lumaBlur = Res();

    if (!CreateTex(s, s.scene, w, h, fmt, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST)) return false;
    if (!CreateTex(s, s.lumaH, w, h, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) return false;
    if (!CreateTex(s, s.lumaBlur, w, h, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) return false;

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = fmt;
        s.device->CreateShaderResourceView(s.scene.r.Get(), &sd, SrvCpu(s, SLOT_SCENE));
        sd.Format = DXGI_FORMAT_R16_FLOAT;
        s.device->CreateShaderResourceView(s.lumaH.r.Get(), &sd, SrvCpu(s, SLOT_LUMAH));
        s.device->CreateShaderResourceView(s.lumaBlur.r.Get(), &sd, SrvCpu(s, SLOT_LUMABLUR));
    }

    if (!heapOk)
    {
        s.rtvHeap.Reset();
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2 + scd.BufferCount;   // lumaH, lumaBlur, then back buffers
        if (FAILED(s.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.rtvHeap)))) return false;
        s.rtvInc = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        s.bufferCount = scd.BufferCount;
    }
    s.device->CreateRenderTargetView(s.lumaH.r.Get(), NULL, RtvCpu(s, 0));
    s.device->CreateRenderTargetView(s.lumaBlur.r.Get(), NULL, RtvCpu(s, 1));

    s.sceneW = w; s.sceneH = h; s.sceneFmt = fmt;
    return true;
}

static void UpdateCbuffer(Fx12& s, UINT w, UINT h, const SurfaceInfo& surf)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = (double)(now.QuadPart - s.qpcLast.QuadPart) * 1000.0 / (double)s.qpcFreq.QuadPart;
    s.qpcLast = now;
    if (dt > 1000.0 || dt <= 0.0) dt = 16.6;

    DvhdrCbGpu cb = {};
    Config_FillCbuffer(&cb, w, h, surf, (float)dt);
    memcpy(s.cbMapped, &cb, sizeof(cb));
}

bool Effect12_Apply(IDXGISwapChain* swap, const DXGI_PRESENT_PARAMETERS* present)
{
    ComPtr<ID3D12Device> dev;
    if (FAILED(swap->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return false; // not D3D12

    ComPtr<ID3D12CommandQueue> queue = QueueForDevice(dev.Get());
    if (!queue) return false; // no queue captured for this device yet - present unmodified

    ComPtr<IDXGISwapChain3> sc3;
    if (FAILED(swap->QueryInterface(IID_PPV_ARGS(&sc3)))) return false;

    UINT idx = sc3->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> bb;
    if (FAILED(swap->GetBuffer(idx, IID_PPV_ARGS(&bb))) || !bb) return false;

    D3D12_RESOURCE_DESC bd = bb->GetDesc();
    SurfaceInfo surf = {};
    const char* why = NULL;
    if (!Config_ClassifySurface(swap, bd.Format, &surf, &why))
    {
        Log_Chain(swap, "d3d12", dev.Get(), bd.Format, (UINT)bd.Width, bd.Height, why);
        return false;
    }

    Fx12& s = AcquireSlot(swap);
    s.lastUse = GetTickCount64();
    if (!EnsureDeviceObjects(s, dev.Get()))
    {
        Log_Chain(swap, "d3d12", dev.Get(), bd.Format, (UINT)bd.Width, bd.Height, "failed: device objects");
        return false;
    }
    if (s.queue.Get() != queue.Get())
    {
        WaitGpuIdle(s);          // drain whatever the previous queue still owes us
        s.queue = queue;
    }
    if (!EnsureSizeBound(s, swap, (UINT)bd.Width, bd.Height, bd.Format))
    {
        Log_Chain(swap, "d3d12", dev.Get(), bd.Format, (UINT)bd.Width, bd.Height, "failed: size-bound textures");
        return false;
    }
    if (!EnsureTonemapPso(s, bd.Format))
    {
        Log_Chain(swap, "d3d12", dev.Get(), bd.Format, (UINT)bd.Width, bd.Height, "failed: tonemap PSO");
        return false;
    }
    if (Log_Enabled())
    {
        char verdict[160];
        Config_DescribeSurface(surf, verdict, sizeof(verdict));
        if (present && present->DirtyRectsCount) strncat(verdict, " (partial presents)", sizeof(verdict) - strlen(verdict) - 1);
        Log_Chain(swap, "d3d12", dev.Get(), bd.Format, (UINT)bd.Width, bd.Height, verdict);
    }

    // Reclaim last frame's allocator before reusing it.
    if (s.fenceValue != 0 && s.fence->GetCompletedValue() < s.fenceValue)
    {
        s.fence->SetEventOnCompletion(s.fenceValue, s.fenceEvent);
        WaitForSingleObject(s.fenceEvent, 2000);
    }
    if (FAILED(s.alloc->Reset())) return false;
    if (FAILED(s.list->Reset(s.alloc.Get(), NULL))) return false;

    UpdateCbuffer(s, (UINT)bd.Width, bd.Height, surf);

    ID3D12GraphicsCommandList* list = s.list.Get();

    // Back buffer arrives in PRESENT; bring the scene texture up to date with it.
    // Under partial presentation only the dirty rectangles are fresh (see the
    // D3D11 path), so the scene texture persists and only those are copied.
    TransitionRaw(s, bb.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(s, s.scene, D3D12_RESOURCE_STATE_COPY_DEST);
    bool partial = s.sceneValid && present && present->DirtyRectsCount > 0 && present->pDirtyRects && !present->pScrollRect;
    if (!partial)
    {
        list->CopyResource(s.scene.r.Get(), bb.Get());
        s.sceneValid = true;
    }
    else
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = s.scene.r.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = bb.Get();        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        for (UINT i = 0; i < present->DirtyRectsCount; i++)
        {
            const RECT& r = present->pDirtyRects[i];
            LONG l = r.left < 0 ? 0 : r.left;
            LONG t = r.top  < 0 ? 0 : r.top;
            LONG rt = r.right  > (LONG)bd.Width  ? (LONG)bd.Width  : r.right;
            LONG bt = r.bottom > (LONG)bd.Height ? (LONG)bd.Height : r.bottom;
            if (rt <= l || bt <= t) continue;
            D3D12_BOX box = { (UINT)l, (UINT)t, 0, (UINT)rt, (UINT)bt, 1 };
            list->CopyTextureRegion(&dst, (UINT)l, (UINT)t, 0, &src, &box);
        }
    }
    Transition(s, s.scene, SR_READ);

    ID3D12DescriptorHeap* heaps[] = { s.srvHeap.Get() };
    list->SetDescriptorHeaps(1, heaps);

    // ---- compute: histogram clear / accumulate / adapt ----
    list->SetComputeRootSignature(s.rootSig.Get());
    list->SetComputeRootConstantBufferView(0, s.cbUpload->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(1, SrvGpu(s, SLOT_SCENE));
    list->SetComputeRootDescriptorTable(2, SrvGpu(s, SLOT_HISTUAV));

    list->SetPipelineState(s.psoClear.Get());
    list->Dispatch(1, 1, 1);
    UavBarrier(s, s.hist.r.Get()); UavBarrier(s, s.adapt.r.Get());

    list->SetPipelineState(s.psoAnalyze.Get());
    UINT stride = (g_knobs.AnalyzeStride >= 1) ? (UINT)g_knobs.AnalyzeStride : 1u;
    UINT step = stride * 16u;
    list->Dispatch(((UINT)bd.Width + step - 1u) / step, (bd.Height + step - 1u) / step, 1);
    UavBarrier(s, s.hist.r.Get());

    list->SetPipelineState(s.psoAdapt.Get());
    list->Dispatch(1, 1, 1);
    UavBarrier(s, s.hist.r.Get()); UavBarrier(s, s.adapt.r.Get());

    // hist + adapt are read as SRVs in the tonemap pass.
    Transition(s, s.hist, SR_READ); Transition(s, s.adapt, SR_READ);

    // ---- graphics: separable blur then tonemap ----
    list->SetGraphicsRootSignature(s.rootSig.Get());
    list->SetGraphicsRootConstantBufferView(0, s.cbUpload->GetGPUVirtualAddress());
    list->SetGraphicsRootDescriptorTable(1, SrvGpu(s, SLOT_SCENE));
    list->SetGraphicsRootDescriptorTable(2, SrvGpu(s, SLOT_HISTUAV));
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VIEWPORT vp = { 0.f, 0.f, (float)bd.Width, (float)bd.Height, 0.f, 1.f };
    D3D12_RECT     rc = { 0, 0, (LONG)bd.Width, (LONG)bd.Height };
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &rc);

    Transition(s, s.lumaH, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = RtvCpu(s, 0);
    list->OMSetRenderTargets(1, &rtvH, FALSE, NULL);
    list->SetPipelineState(s.psoBlurH.Get());
    list->DrawInstanced(3, 1, 0, 0);

    Transition(s, s.lumaH, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(s, s.lumaBlur, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvB = RtvCpu(s, 1);
    list->OMSetRenderTargets(1, &rtvB, FALSE, NULL);
    list->SetPipelineState(s.psoBlurV.Get());
    list->DrawInstanced(3, 1, 0, 0);

    Transition(s, s.lumaBlur, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    TransitionRaw(s, bb.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    UINT bbSlot = 2 + idx;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvBB = RtvCpu(s, bbSlot);
    s.device->CreateRenderTargetView(bb.Get(), NULL, rtvBB);
    list->OMSetRenderTargets(1, &rtvBB, FALSE, NULL);
    list->SetPipelineState(s.psoTonemap.Get());
    list->DrawInstanced(3, 1, 0, 0);

    // Restore states for next frame / present.
    TransitionRaw(s, bb.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    Transition(s, s.hist, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(s, s.adapt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(s, s.scene, D3D12_RESOURCE_STATE_COPY_DEST);

    if (FAILED(list->Close())) return false;
    ID3D12CommandList* lists[] = { list };
    s.queue->ExecuteCommandLists(1, lists);
    s.queue->Signal(s.fence.Get(), ++s.fenceValue);
    return true;
}
