// effect_d3d11.cpp - D3D11 realisation of the DVHDR six-pass tonemap, applied
// to a game's back buffer at Present time. The pass graph mirrors the DWM
// payload (histogram -> temporal adapt -> separable luma blur -> BT.2390
// tonemap), but here it sweeps the whole frame (no DWM dirty rects) and the
// scene-copy texture takes the back buffer's own format so CopyResource is legal
// for scRGB FP16, HDR10 R10G10B10A2 and 8-bit SDR surfaces alike. Full
// render-state is saved and restored around the pass so the game's pipeline is
// never disturbed.
//
// State lives per swap chain. An application may present several chains from
// several threads on several devices (a video player's picture chain beside its
// UI toolkit's chain, say), and each needs its own device objects and its own
// temporal analysis state; sharing one set between them tears resources down
// under a frame in flight. The caller (hook.cpp) serialises Apply and OnResize.

#include "effect_d3d11.h"
#include "config.h"
#include "log.h"
#include <d3d11_4.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#define RELEASE_IF_NOT_NULL(x) { if (x != NULL) { x->Release(); x = NULL; } }

// SM 5.0 bytecode - same shader source as the DWM payload, compiled by the
// CustomBuild step into $(IntDir) (on the include path).
#include "dvhdr_dwm_vs_post.h"
#include "dvhdr_dwm_ps_blurh.h"
#include "dvhdr_dwm_ps_blurv.h"
#include "dvhdr_dwm_ps_tonemap.h"
#include "dvhdr_dwm_cs_clear.h"
#include "dvhdr_dwm_cs_analyze.h"
#include "dvhdr_dwm_cs_adapt.h"

struct Fx11
{
    IDXGISwapChain*      key     = NULL;   // identity only: never dereferenced, never retained
    ULONGLONG            lastUse = 0;

    ID3D11Device*        device  = NULL;
    ID3D11DeviceContext* context = NULL;
    ID3D11Multithread*   mt      = NULL;   // the device's own critical section, when it has one

    ID3D11VertexShader*  vsPost    = NULL;
    ID3D11PixelShader*   psBlurH   = NULL;
    ID3D11PixelShader*   psBlurV   = NULL;
    ID3D11PixelShader*   psTonemap = NULL;
    ID3D11ComputeShader* csClear   = NULL;
    ID3D11ComputeShader* csAnalyze = NULL;
    ID3D11ComputeShader* csAdapt   = NULL;

    ID3D11Buffer*          cbuffer       = NULL;
    ID3D11SamplerState*    sampler       = NULL;
    ID3D11RasterizerState* rasterScissor = NULL;
    ID3D11BlendState*      blendOpaque   = NULL;
    ID3D11DepthStencilState* depthOff    = NULL;

    ID3D11Texture2D*           histTex  = NULL;
    ID3D11ShaderResourceView*  histSRV  = NULL;
    ID3D11UnorderedAccessView* histUAV  = NULL;
    ID3D11Texture2D*           adaptTex = NULL;
    ID3D11ShaderResourceView*  adaptSRV = NULL;
    ID3D11UnorderedAccessView* adaptUAV = NULL;

    UINT        sceneW = 0, sceneH = 0;
    DXGI_FORMAT sceneFmt = DXGI_FORMAT_UNKNOWN;
    bool        sceneValid = false;   // holds a complete untonemapped frame
    ID3D11Texture2D*          sceneTex    = NULL;
    ID3D11ShaderResourceView* sceneSRV    = NULL;
    ID3D11Texture2D*          lumaHTex    = NULL;
    ID3D11RenderTargetView*   lumaHRTV    = NULL;
    ID3D11ShaderResourceView* lumaHSRV    = NULL;
    ID3D11Texture2D*          lumaBlurTex = NULL;
    ID3D11RenderTargetView*   lumaBlurRTV = NULL;
    ID3D11ShaderResourceView* lumaBlurSRV = NULL;

    LARGE_INTEGER qpcFreq = {};
    LARGE_INTEGER qpcLast = {};
    bool pipelineReady = false;
};

static const int kMaxSlots = 4;
static Fx11 g_slots[kMaxSlots];

static void TeardownSizeBound(Fx11& s)
{
    RELEASE_IF_NOT_NULL(s.lumaBlurSRV)
    RELEASE_IF_NOT_NULL(s.lumaBlurRTV)
    RELEASE_IF_NOT_NULL(s.lumaBlurTex)
    RELEASE_IF_NOT_NULL(s.lumaHSRV)
    RELEASE_IF_NOT_NULL(s.lumaHRTV)
    RELEASE_IF_NOT_NULL(s.lumaHTex)
    RELEASE_IF_NOT_NULL(s.sceneSRV)
    RELEASE_IF_NOT_NULL(s.sceneTex)
    s.sceneW = s.sceneH = 0;
    s.sceneFmt = DXGI_FORMAT_UNKNOWN;
    s.sceneValid = false;
}

static void TeardownPipeline(Fx11& s)
{
    TeardownSizeBound(s);
    RELEASE_IF_NOT_NULL(s.adaptUAV)
    RELEASE_IF_NOT_NULL(s.adaptSRV)
    RELEASE_IF_NOT_NULL(s.adaptTex)
    RELEASE_IF_NOT_NULL(s.histUAV)
    RELEASE_IF_NOT_NULL(s.histSRV)
    RELEASE_IF_NOT_NULL(s.histTex)
    RELEASE_IF_NOT_NULL(s.depthOff)
    RELEASE_IF_NOT_NULL(s.blendOpaque)
    RELEASE_IF_NOT_NULL(s.rasterScissor)
    RELEASE_IF_NOT_NULL(s.sampler)
    RELEASE_IF_NOT_NULL(s.cbuffer)
    RELEASE_IF_NOT_NULL(s.csAdapt)
    RELEASE_IF_NOT_NULL(s.csAnalyze)
    RELEASE_IF_NOT_NULL(s.csClear)
    RELEASE_IF_NOT_NULL(s.psTonemap)
    RELEASE_IF_NOT_NULL(s.psBlurV)
    RELEASE_IF_NOT_NULL(s.psBlurH)
    RELEASE_IF_NOT_NULL(s.vsPost)
    s.pipelineReady = false;
}

static void TeardownDevice(Fx11& s)
{
    TeardownPipeline(s);
    RELEASE_IF_NOT_NULL(s.mt)
    RELEASE_IF_NOT_NULL(s.context)
    RELEASE_IF_NOT_NULL(s.device)
}

static void ReleaseSlot(Fx11& s)
{
    TeardownDevice(s);
    s.key = NULL;
    s.lastUse = 0;
}

static Fx11* FindSlot(IDXGISwapChain* key)
{
    for (int i = 0; i < kMaxSlots; i++)
        if (g_slots[i].key == key) return &g_slots[i];
    return NULL;
}

// The slot for this swap chain, else a free one, else the one presented to
// least recently (torn down first).
static Fx11& AcquireSlot(IDXGISwapChain* key)
{
    Fx11* s = FindSlot(key);
    if (s) return *s;

    Fx11* victim = &g_slots[0];
    for (int i = 0; i < kMaxSlots; i++)
    {
        if (!g_slots[i].key) { victim = &g_slots[i]; break; }
        if (g_slots[i].lastUse < victim->lastUse) victim = &g_slots[i];
    }
    ReleaseSlot(*victim);
    victim->key = key;
    return *victim;
}

void Effect11_Shutdown()
{
    for (int i = 0; i < kMaxSlots; i++) ReleaseSlot(g_slots[i]);
}

void Effect11_OnResize(IDXGISwapChain* swap)
{
    Fx11* s = FindSlot(swap);
    if (s) TeardownSizeBound(*s);
}

static bool CreateShaders(Fx11& s)
{
    HRESULT hr = S_OK;
    hr |= s.device->CreateVertexShader (g_VS_Post,    sizeof(g_VS_Post),    NULL, &s.vsPost);
    hr |= s.device->CreatePixelShader  (g_PS_BlurH,   sizeof(g_PS_BlurH),   NULL, &s.psBlurH);
    hr |= s.device->CreatePixelShader  (g_PS_BlurV,   sizeof(g_PS_BlurV),   NULL, &s.psBlurV);
    hr |= s.device->CreatePixelShader  (g_PS_Tonemap, sizeof(g_PS_Tonemap), NULL, &s.psTonemap);
    hr |= s.device->CreateComputeShader(g_CS_Clear,   sizeof(g_CS_Clear),   NULL, &s.csClear);
    hr |= s.device->CreateComputeShader(g_CS_Analyze, sizeof(g_CS_Analyze), NULL, &s.csAnalyze);
    hr |= s.device->CreateComputeShader(g_CS_Adapt,   sizeof(g_CS_Adapt),   NULL, &s.csAdapt);
    return SUCCEEDED(hr);
}

static bool CreateDeviceResources(Fx11& s)
{
    {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = sizeof(DvhdrCbGpu);
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(s.device->CreateBuffer(&d, NULL, &s.cbuffer))) return false;
    }
    {
        D3D11_SAMPLER_DESC d = {};
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        d.ComparisonFunc = D3D11_COMPARISON_NEVER;
        d.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(s.device->CreateSamplerState(&d, &s.sampler))) return false;
    }
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = 256; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32_SINT; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(s.device->CreateTexture2D(&d, NULL, &s.histTex))) return false;
        if (FAILED(s.device->CreateShaderResourceView(s.histTex, NULL, &s.histSRV))) return false;
        if (FAILED(s.device->CreateUnorderedAccessView(s.histTex, NULL, &s.histUAV))) return false;
    }
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = 1; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(s.device->CreateTexture2D(&d, NULL, &s.adaptTex))) return false;
        if (FAILED(s.device->CreateShaderResourceView(s.adaptTex, NULL, &s.adaptSRV))) return false;
        if (FAILED(s.device->CreateUnorderedAccessView(s.adaptTex, NULL, &s.adaptUAV))) return false;
        float zero[4] = {0,0,0,0};
        D3D11_BOX box = { 0,0,0, 1,1,1 };
        s.context->UpdateSubresource(s.adaptTex, 0, &box, zero, sizeof(zero), sizeof(zero));
    }
    {
        D3D11_RASTERIZER_DESC d = {};
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;
        d.DepthClipEnable = TRUE;
        d.ScissorEnable = TRUE;
        if (FAILED(s.device->CreateRasterizerState(&d, &s.rasterScissor))) return false;
    }
    // Our draws must replace pixels outright, whatever blend or depth state the
    // game left bound at Present.
    {
        D3D11_BLEND_DESC d = {};
        d.RenderTarget[0].BlendEnable = FALSE;
        d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(s.device->CreateBlendState(&d, &s.blendOpaque))) return false;
    }
    {
        D3D11_DEPTH_STENCIL_DESC d = {};
        d.DepthEnable = FALSE;
        d.StencilEnable = FALSE;
        if (FAILED(s.device->CreateDepthStencilState(&d, &s.depthOff))) return false;
    }
    return true;
}

// Exact match only: CopyResource demands identical dimensions, so a chain that
// shrinks must not keep the larger textures.
static bool EnsureSizeBound(Fx11& s, UINT W, UINT H, DXGI_FORMAT fmt)
{
    if (fmt == s.sceneFmt && W == s.sceneW && H == s.sceneH
        && s.sceneTex && s.lumaHTex && s.lumaBlurTex) return true;

    TeardownSizeBound(s);

    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(s.device->CreateTexture2D(&d, NULL, &s.sceneTex))) return false;
        if (FAILED(s.device->CreateShaderResourceView(s.sceneTex, NULL, &s.sceneSRV))) return false;
    }
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R16_FLOAT; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(s.device->CreateTexture2D(&d, NULL, &s.lumaHTex))) return false;
        if (FAILED(s.device->CreateRenderTargetView(s.lumaHTex, NULL, &s.lumaHRTV))) return false;
        if (FAILED(s.device->CreateShaderResourceView(s.lumaHTex, NULL, &s.lumaHSRV))) return false;
        if (FAILED(s.device->CreateTexture2D(&d, NULL, &s.lumaBlurTex))) return false;
        if (FAILED(s.device->CreateRenderTargetView(s.lumaBlurTex, NULL, &s.lumaBlurRTV))) return false;
        if (FAILED(s.device->CreateShaderResourceView(s.lumaBlurTex, NULL, &s.lumaBlurSRV))) return false;
    }
    s.sceneW = W; s.sceneH = H; s.sceneFmt = fmt;
    return true;
}

static void UpdateCbuffer(Fx11& s, UINT W, UINT H, const SurfaceInfo& surf)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = (double)(now.QuadPart - s.qpcLast.QuadPart) * 1000.0 / (double)s.qpcFreq.QuadPart;
    s.qpcLast = now;
    if (dt > 1000.0 || dt <= 0.0) dt = 16.6;

    DvhdrCbGpu cb = {};
    Config_FillCbuffer(&cb, W, H, surf, (float)dt);

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(s.context->Map(s.cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, &cb, sizeof(cb));
        s.context->Unmap(s.cbuffer, 0);
    }
}

// A half-built pipeline is torn down again so the next frame starts clean
// instead of running with holes in it.
static bool EnsurePipeline(Fx11& s)
{
    if (s.pipelineReady) return true;
    if (!CreateShaders(s) || !CreateDeviceResources(s))
    {
        TeardownPipeline(s);
        return false;
    }
    s.pipelineReady = true;
    return true;
}

// --- full immediate-context state backup / restore (ImGui-style, scoped to
//     exactly the slots our pass touches) ---------------------------------
struct CtxState
{
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11InputLayout*       inputLayout;
    ID3D11RasterizerState*   raster;
    UINT                     numVP; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT                     numSc; D3D11_RECT     sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11VertexShader*      vs;
    ID3D11PixelShader*       ps;
    ID3D11ComputeShader*     cs;
    ID3D11Buffer*            vsCB, *psCB, *csCB;
    ID3D11SamplerState*      psSamp;
    ID3D11ShaderResourceView* psSRV[5];
    ID3D11ShaderResourceView* csSRV[1];
    ID3D11UnorderedAccessView* csUAV[2];
    ID3D11RenderTargetView*  rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView*  dsv;
    ID3D11BlendState*        blend; float blendFactor[4]; UINT sampleMask;
    ID3D11DepthStencilState* depth; UINT stencilRef;
    ID3D11GeometryShader*    gs;
    ID3D11HullShader*        hs;
    ID3D11DomainShader*      ds;
};

static void SaveState(Fx11& s, CtxState& st)
{
    ID3D11DeviceContext* c = s.context;
    c->IAGetPrimitiveTopology(&st.topo);
    c->IAGetInputLayout(&st.inputLayout);
    c->RSGetState(&st.raster);
    st.numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetViewports(&st.numVP, st.vp);
    st.numSc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    c->RSGetScissorRects(&st.numSc, st.sc);
    c->VSGetShader(&st.vs, NULL, NULL);
    c->PSGetShader(&st.ps, NULL, NULL);
    c->CSGetShader(&st.cs, NULL, NULL);
    c->VSGetConstantBuffers(0, 1, &st.vsCB);
    c->PSGetConstantBuffers(0, 1, &st.psCB);
    c->CSGetConstantBuffers(0, 1, &st.csCB);
    c->PSGetSamplers(0, 1, &st.psSamp);
    c->PSGetShaderResources(0, 5, st.psSRV);
    c->CSGetShaderResources(0, 1, st.csSRV);
    c->CSGetUnorderedAccessViews(0, 2, st.csUAV);
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, st.rtv, &st.dsv);
    c->OMGetBlendState(&st.blend, st.blendFactor, &st.sampleMask);
    c->OMGetDepthStencilState(&st.depth, &st.stencilRef);
    c->GSGetShader(&st.gs, NULL, NULL);
    c->HSGetShader(&st.hs, NULL, NULL);
    c->DSGetShader(&st.ds, NULL, NULL);
}

static void RestoreState(Fx11& s, CtxState& st)
{
    ID3D11DeviceContext* c = s.context;
    c->IASetPrimitiveTopology(st.topo);
    c->IASetInputLayout(st.inputLayout);                 RELEASE_IF_NOT_NULL(st.inputLayout)
    c->RSSetState(st.raster);                            RELEASE_IF_NOT_NULL(st.raster)
    if (st.numVP) c->RSSetViewports(st.numVP, st.vp);
    if (st.numSc) c->RSSetScissorRects(st.numSc, st.sc);
    c->VSSetShader(st.vs, NULL, 0);                      RELEASE_IF_NOT_NULL(st.vs)
    c->PSSetShader(st.ps, NULL, 0);                      RELEASE_IF_NOT_NULL(st.ps)
    c->CSSetShader(st.cs, NULL, 0);                      RELEASE_IF_NOT_NULL(st.cs)
    c->VSSetConstantBuffers(0, 1, &st.vsCB);            RELEASE_IF_NOT_NULL(st.vsCB)
    c->PSSetConstantBuffers(0, 1, &st.psCB);            RELEASE_IF_NOT_NULL(st.psCB)
    c->CSSetConstantBuffers(0, 1, &st.csCB);            RELEASE_IF_NOT_NULL(st.csCB)
    c->PSSetSamplers(0, 1, &st.psSamp);                 RELEASE_IF_NOT_NULL(st.psSamp)
    c->PSSetShaderResources(0, 5, st.psSRV);
    for (auto* p : st.psSRV) RELEASE_IF_NOT_NULL(p)
    c->CSSetShaderResources(0, 1, st.csSRV);
    for (auto* p : st.csSRV) RELEASE_IF_NOT_NULL(p)
    UINT initial[2] = { (UINT)-1, (UINT)-1 };
    c->CSSetUnorderedAccessViews(0, 2, st.csUAV, initial);
    for (auto* p : st.csUAV) RELEASE_IF_NOT_NULL(p)
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, st.rtv, st.dsv);
    for (auto* p : st.rtv) RELEASE_IF_NOT_NULL(p)
    RELEASE_IF_NOT_NULL(st.dsv)
    c->OMSetBlendState(st.blend, st.blendFactor, st.sampleMask); RELEASE_IF_NOT_NULL(st.blend)
    c->OMSetDepthStencilState(st.depth, st.stencilRef);          RELEASE_IF_NOT_NULL(st.depth)
    c->GSSetShader(st.gs, NULL, 0);                              RELEASE_IF_NOT_NULL(st.gs)
    c->HSSetShader(st.hs, NULL, 0);                              RELEASE_IF_NOT_NULL(st.hs)
    c->DSSetShader(st.ds, NULL, 0);                              RELEASE_IF_NOT_NULL(st.ds)
}

// Bring the scene texture up to date with this present. Under partial
// presentation (Chromium and friends) only the dirty rectangles hold fresh
// pixels; the rest of this back buffer is a stale frame from a few presents
// ago, already tonemapped once. So the scene texture lives on across presents
// as the untonemapped whole and only the dirty parts are refreshed. A scroll
// rectangle moves old content in ways we do not replicate, so it forces a
// whole copy, as does a scene texture that has never been filled.
static void RefreshScene(Fx11& s, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& bbd,
                         const DXGI_PRESENT_PARAMETERS* pp)
{
    bool partial = s.sceneValid && pp && pp->DirtyRectsCount > 0 && pp->pDirtyRects && !pp->pScrollRect;
    if (!partial)
    {
        s.context->CopyResource(s.sceneTex, bb);
        s.sceneValid = true;
        return;
    }
    for (UINT i = 0; i < pp->DirtyRectsCount; i++)
    {
        const RECT& r = pp->pDirtyRects[i];
        LONG l = r.left < 0 ? 0 : r.left;
        LONG t = r.top  < 0 ? 0 : r.top;
        LONG rt = r.right  > (LONG)bbd.Width  ? (LONG)bbd.Width  : r.right;
        LONG bt = r.bottom > (LONG)bbd.Height ? (LONG)bbd.Height : r.bottom;
        if (rt <= l || bt <= t) continue;
        D3D11_BOX box = { (UINT)l, (UINT)t, 0, (UINT)rt, (UINT)bt, 1 };
        s.context->CopySubresourceRegion(s.sceneTex, 0, (UINT)l, (UINT)t, 0, bb, 0, &box);
    }
}

static bool RunPipeline(Fx11& s, ID3D11Texture2D* backBuffer, const SurfaceInfo& surf,
                        const DXGI_PRESENT_PARAMETERS* present, const char** fail)
{
    ID3D11DeviceContext* c = s.context;
    D3D11_TEXTURE2D_DESC bbd; backBuffer->GetDesc(&bbd);
    if (!EnsureSizeBound(s, bbd.Width, bbd.Height, bbd.Format)) { *fail = "failed: size-bound textures"; return false; }

    UpdateCbuffer(s, bbd.Width, bbd.Height, surf);
    RefreshScene(s, backBuffer, bbd, present);

    c->VSSetConstantBuffers(0, 1, &s.cbuffer);
    c->PSSetConstantBuffers(0, 1, &s.cbuffer);
    c->CSSetConstantBuffers(0, 1, &s.cbuffer);

    ID3D11UnorderedAccessView* uavs[] = { s.histUAV, s.adaptUAV };
    c->CSSetUnorderedAccessViews(0, 2, uavs, NULL);
    c->CSSetShader(s.csClear, NULL, 0);
    c->Dispatch(1, 1, 1);

    ID3D11ShaderResourceView* csIn[] = { s.sceneSRV };
    c->CSSetShaderResources(0, 1, csIn);
    c->CSSetShader(s.csAnalyze, NULL, 0);
    UINT stride = (g_knobs.AnalyzeStride >= 1) ? (UINT)g_knobs.AnalyzeStride : 1u;
    UINT step   = stride * 16u;
    c->Dispatch((bbd.Width + step - 1u) / step, (bbd.Height + step - 1u) / step, 1);

    c->CSSetShader(s.csAdapt, NULL, 0);
    c->Dispatch(1, 1, 1);

    ID3D11UnorderedAccessView* nullUav[2] = { NULL, NULL };
    c->CSSetUnorderedAccessViews(0, 2, nullUav, NULL);
    ID3D11ShaderResourceView* nullCsSrv[1] = { NULL };
    c->CSSetShaderResources(0, 1, nullCsSrv);

    D3D11_VIEWPORT vp = { 0.f, 0.f, (float)bbd.Width, (float)bbd.Height, 0.f, 1.f };
    c->RSSetViewports(1, &vp);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->IASetInputLayout(NULL);
    c->VSSetShader(s.vsPost, NULL, 0);
    c->PSSetSamplers(0, 1, &s.sampler);
    float noFactor[4] = { 0.f, 0.f, 0.f, 0.f };
    c->OMSetBlendState(s.blendOpaque, noFactor, 0xFFFFFFFFu);
    c->OMSetDepthStencilState(s.depthOff, 0);
    c->GSSetShader(NULL, NULL, 0);
    c->HSSetShader(NULL, NULL, 0);
    c->DSSetShader(NULL, NULL, 0);

    // Force a full-frame scissor for the blur passes so the neighbourhood mean
    // (the lift's base/zone) is computed across the whole frame, rather than
    // inheriting whatever scissor the game left bound at Present (which could clip
    // the blur and leave the base black, killing the lift at LiftLocality 0).
    c->RSSetState(s.rasterScissor);
    D3D11_RECT full = { 0, 0, (LONG)bbd.Width, (LONG)bbd.Height };
    c->RSSetScissorRects(1, &full);

    ID3D11ShaderResourceView* nullSrvs[5] = { NULL, NULL, NULL, NULL, NULL };

    c->PSSetShaderResources(0, 5, nullSrvs);
    c->PSSetShaderResources(0, 1, &s.sceneSRV);
    c->OMSetRenderTargets(1, &s.lumaHRTV, NULL);
    c->PSSetShader(s.psBlurH, NULL, 0);
    c->Draw(3, 0);

    c->OMSetRenderTargets(0, NULL, NULL);
    c->PSSetShaderResources(0, 5, nullSrvs);
    c->PSSetShaderResources(3, 1, &s.lumaHSRV);
    c->OMSetRenderTargets(1, &s.lumaBlurRTV, NULL);
    c->PSSetShader(s.psBlurV, NULL, 0);
    c->Draw(3, 0);

    c->OMSetRenderTargets(0, NULL, NULL);
    c->PSSetShaderResources(0, 5, nullSrvs);
    ID3D11RenderTargetView* bbRTV = NULL;
    if (FAILED(s.device->CreateRenderTargetView(backBuffer, NULL, &bbRTV)) || !bbRTV) { *fail = "failed: back-buffer RTV"; return false; }
    ID3D11ShaderResourceView* tmIn[5] = { s.sceneSRV, s.histSRV, s.adaptSRV, NULL, s.lumaBlurSRV };
    c->PSSetShaderResources(0, 5, tmIn);
    c->OMSetRenderTargets(1, &bbRTV, NULL);
    c->PSSetShader(s.psTonemap, NULL, 0);
    c->RSSetState(s.rasterScissor);
    c->RSSetScissorRects(1, &full);
    c->Draw(3, 0);
    bbRTV->Release();

    c->OMSetRenderTargets(0, NULL, NULL);
    c->PSSetShaderResources(0, 5, nullSrvs);
    return true;
}

bool Effect11_Apply(IDXGISwapChain* swap, const DXGI_PRESENT_PARAMETERS* present)
{
    ID3D11Device* dev = NULL;
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev)
        return false; // not a D3D11 swap chain (likely D3D12) - let the d3d12 path try

    ID3D11Texture2D* bb = NULL;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb)
    {
        dev->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc; bb->GetDesc(&desc);
    SurfaceInfo surf = {};
    const char* why = NULL;
    if (!Config_ClassifySurface(swap, desc.Format, &surf, &why))
    {
        Log_Chain(swap, "d3d11", dev, desc.Format, desc.Width, desc.Height, why);
        bb->Release(); dev->Release(); return false;
    }

    Fx11& s = AcquireSlot(swap);
    s.lastUse = GetTickCount64();
    if (dev != s.device)
    {
        // First sight of this chain, or its address now belongs to a chain on
        // another device: start over on the device it actually presents from.
        TeardownDevice(s);
        s.device = dev; s.device->AddRef();
        s.device->GetImmediateContext(&s.context);
        s.context->QueryInterface(IID_PPV_ARGS(&s.mt));
        QueryPerformanceFrequency(&s.qpcFreq);
        QueryPerformanceCounter(&s.qpcLast);
        Log_Write("chain %p: device %p immediate context %p, multithread protection %s", swap, dev, s.context,
                  !s.mt ? "unavailable" : (s.mt->GetMultithreadProtected() ? "on" : "off"));
    }
    dev->Release();

    // An application that drives one device from several threads (a video
    // renderer beside its UI toolkit, say) serialises them through the device's
    // critical section. Our pass is a long run of context calls that must not be
    // interleaved with theirs, so hold that section for its whole duration.
    if (s.mt) s.mt->Enter();
    bool ok = false;
    const char* fail = "failed: pipeline objects";
    if (EnsurePipeline(s))
    {
        CtxState st;
        SaveState(s, st);
        ok = RunPipeline(s, bb, surf, present, &fail);
        RestoreState(s, st);
    }
    if (s.mt) s.mt->Leave();
    if (Log_Enabled())
    {
        char verdict[160];
        Config_DescribeSurface(surf, verdict, sizeof(verdict));
        if (present && present->DirtyRectsCount) strncat(verdict, " (partial presents)", sizeof(verdict) - strlen(verdict) - 1);
        Log_Chain(swap, "d3d11", s.device, desc.Format, desc.Width, desc.Height, ok ? verdict : fail);
    }
    bb->Release();
    return ok;
}
