// effect_d3d11.cpp — D3D11 realisation of the DVHDR six-pass tonemap, applied
// to a game's back buffer at Present time. The pass graph mirrors the DWM
// payload (histogram → temporal adapt → separable luma blur → BT.2390 tonemap),
// but here it sweeps the whole frame (no DWM dirty rects) and the scene-copy
// texture takes the back buffer's own format so CopyResource is legal for both
// scRGB FP16 and HDR10 R10G10B10A2 surfaces. Full render-state is saved and
// restored around the pass so the game's pipeline is never disturbed.

#include "effect_d3d11.h"
#include "config.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#define RELEASE_IF_NOT_NULL(x) { if (x != NULL) { x->Release(); x = NULL; } }

// SM 5.0 bytecode — same shader source as the DWM payload, compiled by the
// CustomBuild step into $(IntDir) (on the include path).
#include "dvhdr_dwm_vs_post.h"
#include "dvhdr_dwm_ps_blurh.h"
#include "dvhdr_dwm_ps_blurv.h"
#include "dvhdr_dwm_ps_tonemap.h"
#include "dvhdr_dwm_cs_clear.h"
#include "dvhdr_dwm_cs_analyze.h"
#include "dvhdr_dwm_cs_adapt.h"

static ID3D11Device*        g_device  = NULL;
static ID3D11DeviceContext* g_context = NULL;

static ID3D11VertexShader*  g_vsPost     = NULL;
static ID3D11PixelShader*   g_psBlurH    = NULL;
static ID3D11PixelShader*   g_psBlurV    = NULL;
static ID3D11PixelShader*   g_psTonemap  = NULL;
static ID3D11ComputeShader* g_csClear    = NULL;
static ID3D11ComputeShader* g_csAnalyze  = NULL;
static ID3D11ComputeShader* g_csAdapt    = NULL;

static ID3D11Buffer*          g_cbuffer       = NULL;
static ID3D11SamplerState*    g_sampler       = NULL;
static ID3D11RasterizerState* g_rasterScissor = NULL;

static ID3D11Texture2D*           g_histTex  = NULL;
static ID3D11ShaderResourceView*  g_histSRV  = NULL;
static ID3D11UnorderedAccessView* g_histUAV  = NULL;
static ID3D11Texture2D*           g_adaptTex = NULL;
static ID3D11ShaderResourceView*  g_adaptSRV = NULL;
static ID3D11UnorderedAccessView* g_adaptUAV = NULL;

static UINT        g_sceneW = 0, g_sceneH = 0;
static DXGI_FORMAT g_sceneFmt = DXGI_FORMAT_UNKNOWN;
static ID3D11Texture2D*          g_sceneTex     = NULL;
static ID3D11ShaderResourceView* g_sceneSRV     = NULL;
static ID3D11Texture2D*          g_lumaHTex     = NULL;
static ID3D11RenderTargetView*   g_lumaHRTV     = NULL;
static ID3D11ShaderResourceView* g_lumaHSRV     = NULL;
static ID3D11Texture2D*          g_lumaBlurTex  = NULL;
static ID3D11RenderTargetView*   g_lumaBlurRTV  = NULL;
static ID3D11ShaderResourceView* g_lumaBlurSRV  = NULL;

static LARGE_INTEGER g_qpcFreq = {};
static LARGE_INTEGER g_qpcLast = {};
static bool g_pipelineReady = false;

static void TeardownSizeBound()
{
    RELEASE_IF_NOT_NULL(g_lumaBlurSRV)
    RELEASE_IF_NOT_NULL(g_lumaBlurRTV)
    RELEASE_IF_NOT_NULL(g_lumaBlurTex)
    RELEASE_IF_NOT_NULL(g_lumaHSRV)
    RELEASE_IF_NOT_NULL(g_lumaHRTV)
    RELEASE_IF_NOT_NULL(g_lumaHTex)
    RELEASE_IF_NOT_NULL(g_sceneSRV)
    RELEASE_IF_NOT_NULL(g_sceneTex)
    g_sceneW = g_sceneH = 0;
    g_sceneFmt = DXGI_FORMAT_UNKNOWN;
}

static void TeardownPipeline()
{
    TeardownSizeBound();
    RELEASE_IF_NOT_NULL(g_adaptUAV)
    RELEASE_IF_NOT_NULL(g_adaptSRV)
    RELEASE_IF_NOT_NULL(g_adaptTex)
    RELEASE_IF_NOT_NULL(g_histUAV)
    RELEASE_IF_NOT_NULL(g_histSRV)
    RELEASE_IF_NOT_NULL(g_histTex)
    RELEASE_IF_NOT_NULL(g_rasterScissor)
    RELEASE_IF_NOT_NULL(g_sampler)
    RELEASE_IF_NOT_NULL(g_cbuffer)
    RELEASE_IF_NOT_NULL(g_csAdapt)
    RELEASE_IF_NOT_NULL(g_csAnalyze)
    RELEASE_IF_NOT_NULL(g_csClear)
    RELEASE_IF_NOT_NULL(g_psTonemap)
    RELEASE_IF_NOT_NULL(g_psBlurV)
    RELEASE_IF_NOT_NULL(g_psBlurH)
    RELEASE_IF_NOT_NULL(g_vsPost)
    g_pipelineReady = false;
}

static void TeardownDevice()
{
    TeardownPipeline();
    RELEASE_IF_NOT_NULL(g_context)
    RELEASE_IF_NOT_NULL(g_device)
}

void Effect11_Shutdown() { TeardownDevice(); }
void Effect11_OnResize() { TeardownSizeBound(); }

static bool CreateShaders()
{
    HRESULT hr = S_OK;
    hr |= g_device->CreateVertexShader (g_VS_Post,    sizeof(g_VS_Post),    NULL, &g_vsPost);
    hr |= g_device->CreatePixelShader  (g_PS_BlurH,   sizeof(g_PS_BlurH),   NULL, &g_psBlurH);
    hr |= g_device->CreatePixelShader  (g_PS_BlurV,   sizeof(g_PS_BlurV),   NULL, &g_psBlurV);
    hr |= g_device->CreatePixelShader  (g_PS_Tonemap, sizeof(g_PS_Tonemap), NULL, &g_psTonemap);
    hr |= g_device->CreateComputeShader(g_CS_Clear,   sizeof(g_CS_Clear),   NULL, &g_csClear);
    hr |= g_device->CreateComputeShader(g_CS_Analyze, sizeof(g_CS_Analyze), NULL, &g_csAnalyze);
    hr |= g_device->CreateComputeShader(g_CS_Adapt,   sizeof(g_CS_Adapt),   NULL, &g_csAdapt);
    return SUCCEEDED(hr);
}

static bool CreateDeviceResources()
{
    {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = sizeof(DvhdrCbGpu);
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_device->CreateBuffer(&d, NULL, &g_cbuffer))) return false;
    }
    {
        D3D11_SAMPLER_DESC d = {};
        d.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        d.AddressU = d.AddressV = d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        d.ComparisonFunc = D3D11_COMPARISON_NEVER;
        d.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(g_device->CreateSamplerState(&d, &g_sampler))) return false;
    }
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = 256; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32_SINT; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(g_device->CreateTexture2D(&d, NULL, &g_histTex))) return false;
        if (FAILED(g_device->CreateShaderResourceView(g_histTex, NULL, &g_histSRV))) return false;
        if (FAILED(g_device->CreateUnorderedAccessView(g_histTex, NULL, &g_histUAV))) return false;
    }
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = 1; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(g_device->CreateTexture2D(&d, NULL, &g_adaptTex))) return false;
        if (FAILED(g_device->CreateShaderResourceView(g_adaptTex, NULL, &g_adaptSRV))) return false;
        if (FAILED(g_device->CreateUnorderedAccessView(g_adaptTex, NULL, &g_adaptUAV))) return false;
        float zero[4] = {0,0,0,0};
        D3D11_BOX box = { 0,0,0, 1,1,1 };
        g_context->UpdateSubresource(g_adaptTex, 0, &box, zero, sizeof(zero), sizeof(zero));
    }
    {
        D3D11_RASTERIZER_DESC d = {};
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;
        d.DepthClipEnable = TRUE;
        d.ScissorEnable = TRUE;
        if (FAILED(g_device->CreateRasterizerState(&d, &g_rasterScissor))) return false;
    }
    return true;
}

static bool EnsureSizeBound(UINT W, UINT H, DXGI_FORMAT fmt)
{
    if (fmt == g_sceneFmt && W <= g_sceneW && H <= g_sceneH
        && g_sceneTex && g_lumaHTex && g_lumaBlurTex) return true;

    TeardownSizeBound();

    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = fmt; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_device->CreateTexture2D(&d, NULL, &g_sceneTex))) return false;
        if (FAILED(g_device->CreateShaderResourceView(g_sceneTex, NULL, &g_sceneSRV))) return false;
    }
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R16_FLOAT; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_device->CreateTexture2D(&d, NULL, &g_lumaHTex))) return false;
        if (FAILED(g_device->CreateRenderTargetView(g_lumaHTex, NULL, &g_lumaHRTV))) return false;
        if (FAILED(g_device->CreateShaderResourceView(g_lumaHTex, NULL, &g_lumaHSRV))) return false;
        if (FAILED(g_device->CreateTexture2D(&d, NULL, &g_lumaBlurTex))) return false;
        if (FAILED(g_device->CreateRenderTargetView(g_lumaBlurTex, NULL, &g_lumaBlurRTV))) return false;
        if (FAILED(g_device->CreateShaderResourceView(g_lumaBlurTex, NULL, &g_lumaBlurSRV))) return false;
    }
    g_sceneW = W; g_sceneH = H; g_sceneFmt = fmt;
    return true;
}

static void UpdateCbuffer(UINT W, UINT H, UINT colorSpace)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = (double)(now.QuadPart - g_qpcLast.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
    g_qpcLast = now;
    if (dt > 1000.0 || dt <= 0.0) dt = 16.6;

    DvhdrCbGpu cb = {};
    Config_FillCbuffer(&cb, W, H, colorSpace, (float)dt);

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_context->Map(g_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, &cb, sizeof(cb));
        g_context->Unmap(g_cbuffer, 0);
    }
}

static bool EnsurePipeline()
{
    if (g_pipelineReady) return true;
    if (!CreateShaders())         return false;
    if (!CreateDeviceResources()) return false;
    g_pipelineReady = true;
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
};

static void SaveState(CtxState& s)
{
    g_context->IAGetPrimitiveTopology(&s.topo);
    g_context->IAGetInputLayout(&s.inputLayout);
    g_context->RSGetState(&s.raster);
    s.numVP = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_context->RSGetViewports(&s.numVP, s.vp);
    s.numSc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_context->RSGetScissorRects(&s.numSc, s.sc);
    g_context->VSGetShader(&s.vs, NULL, NULL);
    g_context->PSGetShader(&s.ps, NULL, NULL);
    g_context->CSGetShader(&s.cs, NULL, NULL);
    g_context->VSGetConstantBuffers(0, 1, &s.vsCB);
    g_context->PSGetConstantBuffers(0, 1, &s.psCB);
    g_context->CSGetConstantBuffers(0, 1, &s.csCB);
    g_context->PSGetSamplers(0, 1, &s.psSamp);
    g_context->PSGetShaderResources(0, 5, s.psSRV);
    g_context->CSGetShaderResources(0, 1, s.csSRV);
    g_context->CSGetUnorderedAccessViews(0, 2, s.csUAV);
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv);
}

static void RestoreState(CtxState& s)
{
    g_context->IASetPrimitiveTopology(s.topo);
    g_context->IASetInputLayout(s.inputLayout);                 RELEASE_IF_NOT_NULL(s.inputLayout)
    g_context->RSSetState(s.raster);                            RELEASE_IF_NOT_NULL(s.raster)
    if (s.numVP) g_context->RSSetViewports(s.numVP, s.vp);
    if (s.numSc) g_context->RSSetScissorRects(s.numSc, s.sc);
    g_context->VSSetShader(s.vs, NULL, 0);                      RELEASE_IF_NOT_NULL(s.vs)
    g_context->PSSetShader(s.ps, NULL, 0);                      RELEASE_IF_NOT_NULL(s.ps)
    g_context->CSSetShader(s.cs, NULL, 0);                      RELEASE_IF_NOT_NULL(s.cs)
    g_context->VSSetConstantBuffers(0, 1, &s.vsCB);            RELEASE_IF_NOT_NULL(s.vsCB)
    g_context->PSSetConstantBuffers(0, 1, &s.psCB);            RELEASE_IF_NOT_NULL(s.psCB)
    g_context->CSSetConstantBuffers(0, 1, &s.csCB);            RELEASE_IF_NOT_NULL(s.csCB)
    g_context->PSSetSamplers(0, 1, &s.psSamp);                 RELEASE_IF_NOT_NULL(s.psSamp)
    g_context->PSSetShaderResources(0, 5, s.psSRV);
    for (auto* p : s.psSRV) RELEASE_IF_NOT_NULL(p)
    g_context->CSSetShaderResources(0, 1, s.csSRV);
    for (auto* p : s.csSRV) RELEASE_IF_NOT_NULL(p)
    UINT initial[2] = { (UINT)-1, (UINT)-1 };
    g_context->CSSetUnorderedAccessViews(0, 2, s.csUAV, initial);
    for (auto* p : s.csUAV) RELEASE_IF_NOT_NULL(p)
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
    for (auto* p : s.rtv) RELEASE_IF_NOT_NULL(p)
    RELEASE_IF_NOT_NULL(s.dsv)
}

static bool RunPipeline(ID3D11Texture2D* backBuffer, UINT colorSpace)
{
    D3D11_TEXTURE2D_DESC bbd; backBuffer->GetDesc(&bbd);
    if (!EnsureSizeBound(bbd.Width, bbd.Height, bbd.Format)) return false;

    UpdateCbuffer(bbd.Width, bbd.Height, colorSpace);
    g_context->CopyResource(g_sceneTex, backBuffer);

    g_context->VSSetConstantBuffers(0, 1, &g_cbuffer);
    g_context->PSSetConstantBuffers(0, 1, &g_cbuffer);
    g_context->CSSetConstantBuffers(0, 1, &g_cbuffer);

    ID3D11UnorderedAccessView* uavs[] = { g_histUAV, g_adaptUAV };
    g_context->CSSetUnorderedAccessViews(0, 2, uavs, NULL);
    g_context->CSSetShader(g_csClear, NULL, 0);
    g_context->Dispatch(1, 1, 1);

    ID3D11ShaderResourceView* csIn[] = { g_sceneSRV };
    g_context->CSSetShaderResources(0, 1, csIn);
    g_context->CSSetShader(g_csAnalyze, NULL, 0);
    UINT stride = (g_knobs.AnalyzeStride >= 1) ? (UINT)g_knobs.AnalyzeStride : 1u;
    UINT step   = stride * 16u;
    g_context->Dispatch((bbd.Width + step - 1u) / step, (bbd.Height + step - 1u) / step, 1);

    g_context->CSSetShader(g_csAdapt, NULL, 0);
    g_context->Dispatch(1, 1, 1);

    ID3D11UnorderedAccessView* nullUav[2] = { NULL, NULL };
    g_context->CSSetUnorderedAccessViews(0, 2, nullUav, NULL);
    ID3D11ShaderResourceView* nullCsSrv[1] = { NULL };
    g_context->CSSetShaderResources(0, 1, nullCsSrv);

    D3D11_VIEWPORT vp = { 0.f, 0.f, (float)bbd.Width, (float)bbd.Height, 0.f, 1.f };
    g_context->RSSetViewports(1, &vp);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->IASetInputLayout(NULL);
    g_context->VSSetShader(g_vsPost, NULL, 0);
    g_context->PSSetSamplers(0, 1, &g_sampler);

    ID3D11ShaderResourceView* nullSrvs[5] = { NULL, NULL, NULL, NULL, NULL };

    g_context->PSSetShaderResources(0, 5, nullSrvs);
    g_context->PSSetShaderResources(0, 1, &g_sceneSRV);
    g_context->OMSetRenderTargets(1, &g_lumaHRTV, NULL);
    g_context->PSSetShader(g_psBlurH, NULL, 0);
    g_context->Draw(3, 0);

    g_context->OMSetRenderTargets(0, NULL, NULL);
    g_context->PSSetShaderResources(0, 5, nullSrvs);
    g_context->PSSetShaderResources(3, 1, &g_lumaHSRV);
    g_context->OMSetRenderTargets(1, &g_lumaBlurRTV, NULL);
    g_context->PSSetShader(g_psBlurV, NULL, 0);
    g_context->Draw(3, 0);

    g_context->OMSetRenderTargets(0, NULL, NULL);
    g_context->PSSetShaderResources(0, 5, nullSrvs);
    ID3D11RenderTargetView* bbRTV = NULL;
    if (FAILED(g_device->CreateRenderTargetView(backBuffer, NULL, &bbRTV)) || !bbRTV) return false;
    ID3D11ShaderResourceView* tmIn[5] = { g_sceneSRV, g_histSRV, g_adaptSRV, NULL, g_lumaBlurSRV };
    g_context->PSSetShaderResources(0, 5, tmIn);
    g_context->OMSetRenderTargets(1, &bbRTV, NULL);
    g_context->PSSetShader(g_psTonemap, NULL, 0);
    g_context->RSSetState(g_rasterScissor);
    D3D11_RECT full = { 0, 0, (LONG)bbd.Width, (LONG)bbd.Height };
    g_context->RSSetScissorRects(1, &full);
    g_context->Draw(3, 0);
    bbRTV->Release();

    g_context->OMSetRenderTargets(0, NULL, NULL);
    g_context->PSSetShaderResources(0, 5, nullSrvs);
    return true;
}

bool Effect11_Apply(IDXGISwapChain* swap)
{
    ID3D11Device* dev = NULL;
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev)
        return false; // not a D3D11 swap chain (likely D3D12) — let the d3d12 path try

    ID3D11Texture2D* bb = NULL;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb)
    {
        dev->Release();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc; bb->GetDesc(&desc);
    UINT colorSpace = Config_ColorSpaceForFormat(desc.Format);
    if (colorSpace == 0) { bb->Release(); dev->Release(); return false; }

    if (dev != g_device)
    {
        TeardownDevice();
        g_device = dev; g_device->AddRef();
        g_device->GetImmediateContext(&g_context);
        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_qpcLast);
    }
    dev->Release();

    bool ok = false;
    if (EnsurePipeline())
    {
        CtxState st;
        SaveState(st);
        ok = RunPipeline(bb, colorSpace);
        RestoreState(st);
    }
    bb->Release();
    return ok;
}
