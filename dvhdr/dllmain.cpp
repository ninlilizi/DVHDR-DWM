// dvhdr/dllmain.cpp — DWM-injected dynamic tonemapping payload.
//
// Hook plumbing forked from the lauralex/dwm_lut win25h2-pr branch (which is
// what the Release24h2.zip distribution is actually built from — the public
// master does NOT contain the 25H2 patterns and will not match on 25H2).
//
// We hook three DWM internals:
//   COverlayContext::Present                      — every back-buffer present
//   COverlayContext::IsCandidateDirectFlipCompatbile — force false to keep DWM
//   COverlayContext::OverlaysEnabled              — force false on active mons
// Each comes in four flavours: Win10 / Win11 / Win11_24h2 / Win11_25h2. On
// 25H2 the legacy IDXGISwapChain no longer exists at a known offset — the
// back-buffer texture is obtained via a vtable chain off the overlay swap-
// chain pointer instead, and the present signature gains an extra parameter.
//
// The shader pipeline is not yet wired — ApplyDvhdr* currently no-op stubs.

#include "pch.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "dxguid.lib")

#pragma intrinsic(_ReturnAddress)

#define RELEASE_IF_NOT_NULL(x) { if (x != NULL) { x->Release(); x = NULL; } }

// ===========================================================================
// AOB patterns. '?' = wildcard byte. Sourced from win25h2-pr branch.
// ===========================================================================

// ---- legacy Win10 ----
const unsigned char COverlayContext_Present_bytes[] = {
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x40,0x48,
    0x8b,0xb1,0x20,0x2c,0x00,0x00,0x45,0x8b,0xd0,0x48,0x8b,0xfa,0x48,0x8b,0xd9,0x48,
    0x85,0xf6,0x0f,0x85
};
const int IOverlaySwapChain_IDXGISwapChain_offset = -0x118;
const int COverlayContext_DeviceClipBox_offset    = -0x120;
const int IOverlaySwapChain_HardwareProtected_offset = -0xbc;

const unsigned char COverlayContext_IsCandidateDirectFlipCompatbile_bytes[] = {
    0x48,0x89,0x7c,0x24,0x20,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8b,
    0xec,0x48,0x83,0xec,0x40
};
const unsigned char COverlayContext_OverlaysEnabled_bytes[] = {
    0x75,0x04,0x32,0xc0,0xc3,0xcc,0x83,0x79,0x30,0x01,0x0f,0x97,0xc0,0xc3
};

// ---- Win11 (initial 22000) ----
const unsigned char COverlayContext_Present_bytes_w11[] = {
    0x40,0x53,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xEC,0x88,0x00,0x00,0x00,
    0x48,0x8B,0x05,'?','?','?','?',0x48,0x33,0xC4,0x48,0x89,0x44,0x24,0x78,0x48
};
const int IOverlaySwapChain_IDXGISwapChain_offset_w11 = 0xE0;
const int IOverlaySwapChain_HardwareProtected_offset_w11 = -0x144;
int COverlayContext_DeviceClipBox_offset_w11 = 0x466C;

const unsigned char COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11[] = {
    0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8B,0xEC,
    0x48,0x83,0xEC,0x68,0x48
};
const unsigned char COverlayContext_OverlaysEnabled_bytes_w11[] = {
    0x83,0x3D,'?','?','?','?','?',0x75,0x04
};

// ---- Win11 24H2 ----
const unsigned char COverlayContext_Present_bytes_w11_24h2[] = {
    0x4C,0x8B,0xDC,0x56,0x41,0x56
};
const int IOverlaySwapChain_IDXGISwapChain_offset_w11_24h2 = 0x108;
const int IOverlaySwapChain_HardwareProtected_offset_w11_24h2 = 0x64;
int COverlayContext_DeviceClipBox_offset_w11_24h2 = 0x53E8;

const unsigned char COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_24h2[] = {
    0x48,0x8B,0xC4,0x48,0x89,0x58,'?',0x48,0x89,0x68,'?',0x48,0x89,0x70,'?',0x48,
    0x89,0x78,'?',0x41,0x56,0x48,0x83,0xEC,0x20,0x33,0xDB
};
// _relative — anchor on `E8 ?? ?? ?? ?? 84 C0 B8 04 00 00 00` (call site); the
// real OverlaysEnabled is the call target found via the rel32 displacement.
const unsigned char COverlayContext_OverlaysEnabled_bytes_relative_w11_24h2[] = {
    0xE8,'?','?','?','?',0x84,0xC0,0xB8,0x04,0x00,0x00,0x00
};

// ---- Win11 25H2 ----
const unsigned char COverlayContext_Present_bytes_w11_25h2[] = {
    0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,
    0x24,0xF9,0x48,0x81,0xEC,0xF8,0x00,0x00,0x00,0x48,0x8B,0x05,
    '?','?','?','?',0x48,0x33,0xC4,0x48,0x89,0x45,0xEF,0x4C,0x8B,0x65,'?',0x48,0x8B,0xD9
};
const unsigned char COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_25h2[] = {
    0x48,0x8B,0xC4,0x48,0x89,0x58,0x08,0x48,0x89,0x68,0x10,0x48,0x89,0x70,0x18,0x48,
    0x89,0x78,0x20,0x41,0x56,0x48,0x83,0xEC,0x20,0x33,0xDB
};
const unsigned char COverlayContext_OverlaysEnabled_bytes_w11_25h2[] = {
    0x83,0x3D,'?','?','?','?',0x05,0x74,0x09,0x83,0x79,0x28,0x01,0x0F,0x97,0xC0,0xC3
};
// Access path matches the reference: realObj = *(void**)context;
//                                    rect    = (float*)(realObj + offset);
//                                    left/top = (int)rect[0], (int)rect[1].
int COverlayContext_DeviceClipBox_offset_w11_25h2 = 0x7698;
const int IOverlaySwapChain_HardwareProtected_offset_w11_25h2 = 0x4C;
const int IOverlaySwapChain_GetSwapChain_vtable_offset_w11_25h2 = 0x108;

bool isWindows11      = false;
bool isWindows11_24h2 = false;
bool isWindows11_25h2 = false;

// 25H2: pointer into DWM's OverlayTestMode global. Extracted from the
// OverlaysEnabled AOB match site (the `83 3D imm32 05` cmp instruction
// encodes a RIP-relative displacement to the global). Setting *g_pOverlayTestMode = 5
// disables DirectFlip/MPO process-wide.
static int* g_pOverlayTestMode = NULL;

static bool aob_match_inverse(const void* buf, const void* mask, int len)
{
    auto b = (const unsigned char*)buf;
    auto m = (const unsigned char*)mask;
    for (int i = 0; i < len; i++)
        if (b[i] != m[i] && m[i] != '?') return true;
    return false;
}

// Decode a CALL rel32 (E8 disp32) at `addr+offset` and return the target.
static void* get_relative_address(void* addr, int operand_offset, int instr_size)
{
    int rel = *(int*)((unsigned char*)addr + operand_offset);
    return (unsigned char*)addr + instr_size + rel;
}

// ===========================================================================
// Monitor selection — HKLM\SOFTWARE\DVHDR-DWM\Monitors as REG_MULTI_SZ.
// ===========================================================================

// Per-monitor GPU resources. These MUST NOT be shared across screens: the
// size-bound scene/blur textures have to match each monitor's own back-buffer
// dimensions (a global set sized grow-only made CopyResource silently fail on a
// differently-sized screen and painted one monitor's image onto another), and
// the histogram/adapt state must be independent so each screen adapts to its own
// content instead of fighting over a shared EMA. qpcLast tracks its own cadence.
struct PerMonitor
{
    UINT W, H;
    ID3D11Texture2D*             sceneTex;    ID3D11ShaderResourceView*   sceneSRV;
    ID3D11Texture2D*             lumaHTex;    ID3D11RenderTargetView*     lumaHRTV;     ID3D11ShaderResourceView* lumaHSRV;
    ID3D11Texture2D*             lumaBlurTex; ID3D11RenderTargetView*     lumaBlurRTV;  ID3D11ShaderResourceView* lumaBlurSRV;
    ID3D11Texture2D*             histTex;     ID3D11ShaderResourceView*   histSRV;      ID3D11UnorderedAccessView* histUAV;
    ID3D11Texture2D*             adaptTex;    ID3D11ShaderResourceView*   adaptSRV;     ID3D11UnorderedAccessView* adaptUAV;
    LARGE_INTEGER               qpcLast;
};

// A targeted output: its virtual-screen origin (the key DWM hands us per context),
// the Windows display number that the loader resolved, the panel capabilities
// resolved for it (each [Display.N] override, falling back to the global
// [Display]), and that screen's own GPU resource set.
struct MonitorTarget
{
    int   left, top;
    int   index;                              // Windows display number; -1 if absent
    float Peak, MaxFALL, Black, BlackLift;    // resolved per-monitor capabilities
    PerMonitor res;                           // this screen's private pipeline resources
};
static std::vector<MonitorTarget> g_targets;

static void LoadTargetsFromRegistry()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\DVHDR-DWM", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return;
    DWORD type = 0, cb = 0;
    if (RegQueryValueExW(key, L"Monitors", NULL, &type, NULL, &cb) != ERROR_SUCCESS
        || type != REG_MULTI_SZ || cb < sizeof(wchar_t) * 2)
    {
        RegCloseKey(key);
        return;
    }
    std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 1, 0);
    if (RegQueryValueExW(key, L"Monitors", NULL, &type, (BYTE*)buf.data(), &cb) != ERROR_SUCCESS)
    {
        RegCloseKey(key);
        return;
    }
    RegCloseKey(key);
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1)
    {
        MonitorTarget t = {};
        t.index = -1;                         // entries may be "left,top" or "left,top,index"
        if (swscanf(p, L"%d,%d,%d", &t.left, &t.top, &t.index) >= 2) g_targets.push_back(t);
    }
}

// Per-version DeviceClipBox layout. All modern versions take one level of
// indirection through the context (realObj = *(void**)context), then read
// floats at a version-specific offset. The 24H2 path lives at rect[2..3]
// (the struct has two leading values before left/top); 25H2 and Win11 use
// rect[0..1]. Wrapped in __try so an unexpected memory layout cannot crash
// DWM — we'd rather miss the match than abort the compositor.
static MonitorTarget* GetTargetForContext(void* context)
{
    int left = 0, top = 0;
    __try
    {
        if (isWindows11_25h2)
        {
            void* realObj = *(void**)context;
            if (!realObj) return NULL;
            float* rect = (float*)((unsigned char*)realObj + COverlayContext_DeviceClipBox_offset_w11_25h2);
            left = (int)rect[0]; top = (int)rect[1];
        }
        else if (isWindows11_24h2)
        {
            void* realObj = *(void**)context;
            if (!realObj) return NULL;
            float* rect = (float*)((unsigned char*)realObj + COverlayContext_DeviceClipBox_offset_w11_24h2);
            left = (int)rect[2]; top = (int)rect[3];
        }
        else if (isWindows11)
        {
            void* realObj = *(void**)context;
            if (!realObj) return NULL;
            float* rect = (float*)((unsigned char*)realObj + COverlayContext_DeviceClipBox_offset_w11);
            left = (int)rect[0]; top = (int)rect[1];
        }
        else
        {
            int* rect = (int*)((unsigned char*)context + COverlayContext_DeviceClipBox_offset);
            left = rect[0]; top = rect[1];
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }

    for (auto& t : g_targets)
        if (t.left == left && t.top == top) return &t;
    return NULL;
}

// ===========================================================================
// Active-context set.
// ===========================================================================

static std::vector<void*> g_activeContexts;
static bool IsActive(void* ctx)
{
    for (auto* c : g_activeContexts) if (c == ctx) return true;
    return false;
}
static void SetActive(void* ctx)
{
    if (!IsActive(ctx)) g_activeContexts.push_back(ctx);
}
static void UnsetActive(void* ctx)
{
    for (size_t i = 0; i < g_activeContexts.size(); i++)
        if (g_activeContexts[i] == ctx) { g_activeContexts.erase(g_activeContexts.begin() + i); return; }
}

// ===========================================================================
// INI knobs — read from %SYSTEMROOT%\Temp\dvhdr.ini on attach.
// ===========================================================================

struct DvhdrKnobs
{
    int   ColorSpace;             // 0 auto / 1 scRGB / 2 HDR10
    float DisplayPeak, DisplayMaxFALL, DisplayBlack;
    float HeadroomPercent, MinGain, LiftStrength, MaxGain;
    float HighlightProtect, PeakPercentile;
    float AttackMs, ReleaseMs;
    float DynamicContrast, DetailGain, DetailRadius, DetailBias;
    int   UseHighlightRolloff;
    float Strength;
    int   AnalyzeStride;
    int   DebugOverlay;
    float DitherActivity, DitherStrength, DitherFloor;
    float BlackLift;
    float ShadowToe;
    float ChromaCorrect;
    float LiftLocality;
    float DebandThreshold, DebandRange;
};
static DvhdrKnobs g_knobs;

// GPU-side cbuffer mirror — layout must match `cbuffer DVHDRCb` in
// dvhdr_dwm.hlsl (4-float rows, 16-byte aligned).
struct DvhdrCbGpu
{
    UINT  BufferW, BufferH;
    UINT  ColorSpace;
    float FrameTimeMs;

    float DisplayPeak, DisplayMaxFALL, DisplayBlack, HeadroomPercent;
    float MinGain, LiftStrength, MaxGain, HighlightProtect;
    float PeakPercentile, AttackMs, ReleaseMs, DynamicContrast;
    float DetailGain, DetailRadius, DetailBias;
    UINT  UseHighlightRolloff;
    float Strength;
    UINT  DebugOverlay;
    UINT  AnalyzeStride;
    float DitherActivity;

    float DitherStrength;
    float DitherFloor;
    float BlackLift;
    float ShadowToe;

    float ChromaCorrect;
    float LiftLocality;
    float DebandThreshold, DebandRange;
};
static_assert(sizeof(DvhdrCbGpu) == 128, "cbuffer layout drift");

static float IniFloat(const char* sec, const char* key, float defVal, const char* path)
{
    char buf[64];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), path);
    if (!buf[0]) return defVal;
    return (float)atof(buf);
}

static bool GetIniPath(char* path, size_t cap)
{
    DWORD n = GetWindowsDirectoryA(path, (DWORD)cap);
    if (n == 0 || n + 20 >= cap) return false;
    strcat(path, "\\Temp\\dvhdr.ini");
    return true;
}

static void LoadKnobsFromIni()
{
    char path[MAX_PATH];
    if (!GetIniPath(path, sizeof(path))) return;

    g_knobs.ColorSpace          = GetPrivateProfileIntA("Source",      "ColorSpace",          0,        path);
    g_knobs.DisplayPeak         = IniFloat("Display",   "Peak",                 1300.0f, path);
    g_knobs.DisplayMaxFALL      = IniFloat("Display",   "MaxFALL",              265.0f,  path);
    g_knobs.DisplayBlack        = IniFloat("Display",   "Black",                0.0f,    path);
    g_knobs.BlackLift           = IniFloat("Display",   "BlackLift",            0.00248f, path);
    g_knobs.HeadroomPercent     = IniFloat("Governor",  "HeadroomPercent",      90.0f,   path);
    g_knobs.MinGain             = IniFloat("Governor",  "MinGain",              0.25f,   path);
    g_knobs.LiftStrength        = IniFloat("Governor",  "LiftStrength",         0.25f,   path);
    g_knobs.MaxGain             = IniFloat("Governor",  "MaxGain",              1.5f,    path);
    g_knobs.HighlightProtect    = IniFloat("Governor",  "HighlightProtect",     80.0f,   path);
    g_knobs.ShadowToe           = IniFloat("Governor",  "ShadowToe",            0.25f,   path);
    g_knobs.PeakPercentile      = IniFloat("Governor",  "PeakPercentile",       99.7f,   path);
    g_knobs.AttackMs            = IniFloat("Temporal",  "AttackMs",             80.0f,   path);
    g_knobs.ReleaseMs           = IniFloat("Temporal",  "ReleaseMs",            600.0f,  path);
    g_knobs.DynamicContrast     = IniFloat("ToneCurve", "DynamicContrast",      0.1f,    path);
    g_knobs.DetailGain          = IniFloat("ToneCurve", "DetailGain",           1.0f,    path);
    g_knobs.DetailRadius        = IniFloat("ToneCurve", "DetailRadius",         12.0f,   path);
    g_knobs.DetailBias          = IniFloat("ToneCurve", "DetailBias",           0.0f,    path);
    g_knobs.UseHighlightRolloff = GetPrivateProfileIntA("ToneCurve", "UseHighlightRolloff", 1,        path);
    g_knobs.Strength            = IniFloat("ToneCurve", "Strength",             1.0f,    path);
    g_knobs.AnalyzeStride       = GetPrivateProfileIntA("Performance","AnalyzeStride",        2,        path);
    g_knobs.DebugOverlay        = GetPrivateProfileIntA("Debug",      "Overlay",              0,        path);
    g_knobs.DitherStrength      = IniFloat("Dither",    "Strength",             1.5f,    path);
    g_knobs.DitherActivity      = IniFloat("Dither",    "Activity",             0.002f,  path);
    g_knobs.DitherFloor         = IniFloat("Dither",    "Floor",                0.4f,    path);
    g_knobs.ChromaCorrect       = IniFloat("Color",     "ChromaCorrect",        1.0f,    path);
    g_knobs.LiftLocality        = IniFloat("ToneCurve", "LiftLocality",         0.0f,    path);
    g_knobs.DebandThreshold     = IniFloat("Deband",    "Threshold",            3.0f,    path);
    g_knobs.DebandRange         = IniFloat("Deband",    "Range",                16.0f,   path);
}

// Resolve each target's panel capabilities. Defaults come from the global
// [Display] (already in g_knobs); a per-monitor [Display.N] section (N = the
// Windows display number the loader stamped into the registry) overrides any of
// Peak / MaxFALL / Black / BlackLift for that screen. Must run after both
// LoadTargetsFromRegistry and LoadKnobsFromIni.
static void ResolvePerMonitorCaps()
{
    char path[MAX_PATH];
    bool havePath = GetIniPath(path, sizeof(path));
    for (auto& t : g_targets)
    {
        t.Peak      = g_knobs.DisplayPeak;
        t.MaxFALL   = g_knobs.DisplayMaxFALL;
        t.Black     = g_knobs.DisplayBlack;
        t.BlackLift = g_knobs.BlackLift;
        if (havePath && t.index > 0)
        {
            char sec[32];
            snprintf(sec, sizeof(sec), "Display.%d", t.index);
            t.Peak      = IniFloat(sec, "Peak",      t.Peak,      path);
            t.MaxFALL   = IniFloat(sec, "MaxFALL",   t.MaxFALL,   path);
            t.Black     = IniFloat(sec, "Black",     t.Black,     path);
            t.BlackLift = IniFloat(sec, "BlackLift", t.BlackLift, path);
        }
    }
}

// ===========================================================================
// D3D pipeline state — owned by the DLL, recreated on device change.
// ===========================================================================

static ID3D11Device*        g_device  = NULL;
static ID3D11DeviceContext* g_context = NULL;

static ID3D11VertexShader*  g_vsPost     = NULL;
static ID3D11PixelShader*   g_psBlurH    = NULL;
static ID3D11PixelShader*   g_psBlurV    = NULL;
static ID3D11PixelShader*   g_psTonemap  = NULL;
static ID3D11ComputeShader* g_csClear    = NULL;
static ID3D11ComputeShader* g_csAnalyze  = NULL;
static ID3D11ComputeShader* g_csAdapt    = NULL;

static ID3D11Buffer*        g_cbuffer    = NULL;
static ID3D11SamplerState*  g_sampler    = NULL;
static ID3D11RasterizerState* g_rasterScissor = NULL;

// QueryPerformanceCounter frequency — shared (constant); each monitor keeps its
// own last-tick in PerMonitor::qpcLast.
static LARGE_INTEGER g_qpcFreq = {};

static bool g_pipelineReady = false;

// Release one screen's private resources (size-bound + histogram/adapt).
static void FreeMonitorResources(PerMonitor& pm)
{
    RELEASE_IF_NOT_NULL(pm.lumaBlurSRV)
    RELEASE_IF_NOT_NULL(pm.lumaBlurRTV)
    RELEASE_IF_NOT_NULL(pm.lumaBlurTex)
    RELEASE_IF_NOT_NULL(pm.lumaHSRV)
    RELEASE_IF_NOT_NULL(pm.lumaHRTV)
    RELEASE_IF_NOT_NULL(pm.lumaHTex)
    RELEASE_IF_NOT_NULL(pm.sceneSRV)
    RELEASE_IF_NOT_NULL(pm.sceneTex)
    RELEASE_IF_NOT_NULL(pm.adaptUAV)
    RELEASE_IF_NOT_NULL(pm.adaptSRV)
    RELEASE_IF_NOT_NULL(pm.adaptTex)
    RELEASE_IF_NOT_NULL(pm.histUAV)
    RELEASE_IF_NOT_NULL(pm.histSRV)
    RELEASE_IF_NOT_NULL(pm.histTex)
    pm.W = pm.H = 0;
}

static void TeardownPipeline()
{
    for (auto& t : g_targets) FreeMonitorResources(t.res);
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

static void InitDeviceFromDevice(ID3D11Device* dev)
{
    g_device = dev;
    g_device->AddRef();
    g_device->GetImmediateContext(&g_context);
    QueryPerformanceFrequency(&g_qpcFreq);
}

// Precompiled SM 5.0 bytecode for every entry point. Generated at build time
// by the CustomBuild step in dvhdr.vcxproj — see tools/fxc/fxc.exe. The
// headers land in $(IntDir) which is on the include path.
#include "dvhdr_dwm_vs_post.h"
#include "dvhdr_dwm_ps_blurh.h"
#include "dvhdr_dwm_ps_blurv.h"
#include "dvhdr_dwm_ps_tonemap.h"
#include "dvhdr_dwm_cs_clear.h"
#include "dvhdr_dwm_cs_analyze.h"
#include "dvhdr_dwm_cs_adapt.h"

static bool CompileAndCreateShaders()
{
    HRESULT hr = S_OK;
    hr |= g_device->CreateVertexShader (g_VS_Post,    sizeof(g_VS_Post),    NULL, &g_vsPost);
    hr |= g_device->CreatePixelShader  (g_PS_BlurH,   sizeof(g_PS_BlurH),   NULL, &g_psBlurH);
    hr |= g_device->CreatePixelShader  (g_PS_BlurV,   sizeof(g_PS_BlurV),   NULL, &g_psBlurV);
    hr |= g_device->CreatePixelShader  (g_PS_Tonemap, sizeof(g_PS_Tonemap), NULL, &g_psTonemap);
    hr |= g_device->CreateComputeShader(g_CS_Clear,   sizeof(g_CS_Clear),   NULL, &g_csClear);
    hr |= g_device->CreateComputeShader(g_CS_Analyze, sizeof(g_CS_Analyze), NULL, &g_csAnalyze);
    hr |= g_device->CreateComputeShader(g_CS_Adapt,   sizeof(g_CS_Adapt),   NULL, &g_csAdapt);
    if (FAILED(hr)) return false;
    return true;
}

// One-time, truly shared per-device resources: cbuffer, sampler, rasterizer.
// (Histogram + adapt are per-monitor now — see EnsureMonitorResources.)
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
        // Rasterizer state with scissor enabled — the tonemap pass clips its
        // full-screen triangle to each dirty rect DWM hands us so writes only
        // land in regions that will actually be scanned out.
        D3D11_RASTERIZER_DESC d = {};
        d.FillMode = D3D11_FILL_SOLID;
        d.CullMode = D3D11_CULL_NONE;
        d.DepthClipEnable = TRUE;
        d.ScissorEnable = TRUE;
        if (FAILED(g_device->CreateRasterizerState(&d, &g_rasterScissor))) return false;
    }
    return true;
}

// Ensure one screen's private resources exist and match its back-buffer size.
// The histogram (256x1) + adapt (1x1) are size-independent and created once;
// the scene + blur targets are recreated whenever this screen's dimensions
// change (NOT grow-only — they must match exactly for CopyResource to copy this
// monitor's pixels rather than silently no-op on a size mismatch).
static bool EnsureMonitorResources(PerMonitor& pm, UINT W, UINT H)
{
    if (!pm.histTex)
    {
        {
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = 256; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R32_SINT;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(g_device->CreateTexture2D(&d, NULL, &pm.histTex))) return false;
            if (FAILED(g_device->CreateShaderResourceView(pm.histTex, NULL, &pm.histSRV))) return false;
            if (FAILED(g_device->CreateUnorderedAccessView(pm.histTex, NULL, &pm.histUAV))) return false;
        }
        {
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = 1; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(g_device->CreateTexture2D(&d, NULL, &pm.adaptTex))) return false;
            if (FAILED(g_device->CreateShaderResourceView(pm.adaptTex, NULL, &pm.adaptSRV))) return false;
            if (FAILED(g_device->CreateUnorderedAccessView(pm.adaptTex, NULL, &pm.adaptUAV))) return false;

            // Seed adapt with zeros so the first-frame snap in CS_Adapt works.
            float zero[4] = {0,0,0,0};
            D3D11_BOX box = { 0,0,0, 1,1,1 };
            g_context->UpdateSubresource(pm.adaptTex, 0, &box, zero, sizeof(zero), sizeof(zero));
        }
        QueryPerformanceCounter(&pm.qpcLast);
    }

    if (W == pm.W && H == pm.H && pm.sceneTex && pm.lumaHTex && pm.lumaBlurTex) return true;

    RELEASE_IF_NOT_NULL(pm.lumaBlurSRV)
    RELEASE_IF_NOT_NULL(pm.lumaBlurRTV)
    RELEASE_IF_NOT_NULL(pm.lumaBlurTex)
    RELEASE_IF_NOT_NULL(pm.lumaHSRV)
    RELEASE_IF_NOT_NULL(pm.lumaHRTV)
    RELEASE_IF_NOT_NULL(pm.lumaHTex)
    RELEASE_IF_NOT_NULL(pm.sceneSRV)
    RELEASE_IF_NOT_NULL(pm.sceneTex)

    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_device->CreateTexture2D(&d, NULL, &pm.sceneTex))) return false;
        if (FAILED(g_device->CreateShaderResourceView(pm.sceneTex, NULL, &pm.sceneSRV))) return false;
    }
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R16_FLOAT;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_device->CreateTexture2D(&d, NULL, &pm.lumaHTex))) return false;
        if (FAILED(g_device->CreateRenderTargetView(pm.lumaHTex, NULL, &pm.lumaHRTV))) return false;
        if (FAILED(g_device->CreateShaderResourceView(pm.lumaHTex, NULL, &pm.lumaHSRV))) return false;
        if (FAILED(g_device->CreateTexture2D(&d, NULL, &pm.lumaBlurTex))) return false;
        if (FAILED(g_device->CreateRenderTargetView(pm.lumaBlurTex, NULL, &pm.lumaBlurRTV))) return false;
        if (FAILED(g_device->CreateShaderResourceView(pm.lumaBlurTex, NULL, &pm.lumaBlurSRV))) return false;
    }
    pm.W = W; pm.H = H;
    return true;
}

// Populate the cbuffer with the current frame's knob set. The panel-capability
// fields come from the per-monitor target when one is supplied (multi-screen),
// otherwise from the global knobs. frameTimeMs is this screen's own cadence.
static void UpdateCbuffer(UINT W, UINT H, const MonitorTarget* tgt, float frameTimeMs)
{
    DvhdrCbGpu cb = {};
    cb.BufferW             = W;
    cb.BufferH             = H;
    cb.ColorSpace          = (g_knobs.ColorSpace == 2) ? 2 : 1; // CSP_HDR10 / CSP_SCRGB
    cb.FrameTimeMs         = frameTimeMs;
    cb.DisplayPeak         = tgt ? tgt->Peak    : g_knobs.DisplayPeak;
    cb.DisplayMaxFALL      = tgt ? tgt->MaxFALL : g_knobs.DisplayMaxFALL;
    cb.DisplayBlack        = tgt ? tgt->Black   : g_knobs.DisplayBlack;
    cb.HeadroomPercent     = g_knobs.HeadroomPercent;
    cb.MinGain             = g_knobs.MinGain;
    cb.LiftStrength        = g_knobs.LiftStrength;
    cb.MaxGain             = g_knobs.MaxGain;
    cb.HighlightProtect    = g_knobs.HighlightProtect;
    cb.PeakPercentile      = g_knobs.PeakPercentile;
    cb.AttackMs            = g_knobs.AttackMs;
    cb.ReleaseMs           = g_knobs.ReleaseMs;
    cb.DynamicContrast     = g_knobs.DynamicContrast;
    cb.DetailGain          = g_knobs.DetailGain;
    cb.DetailRadius        = g_knobs.DetailRadius;
    cb.DetailBias          = g_knobs.DetailBias;
    cb.UseHighlightRolloff = (g_knobs.UseHighlightRolloff != 0) ? 1u : 0u;
    cb.Strength            = g_knobs.Strength;
    cb.DebugOverlay        = (UINT)g_knobs.DebugOverlay;
    cb.AnalyzeStride       = (g_knobs.AnalyzeStride >= 1) ? (UINT)g_knobs.AnalyzeStride : 1u;
    cb.DitherActivity      = g_knobs.DitherActivity;
    cb.DitherStrength      = g_knobs.DitherStrength;
    cb.DitherFloor         = g_knobs.DitherFloor;
    cb.BlackLift           = tgt ? tgt->BlackLift : g_knobs.BlackLift;
    cb.ShadowToe           = g_knobs.ShadowToe;
    cb.ChromaCorrect       = g_knobs.ChromaCorrect;
    cb.LiftLocality        = g_knobs.LiftLocality;
    cb.DebandThreshold     = g_knobs.DebandThreshold;
    cb.DebandRange         = g_knobs.DebandRange;

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_context->Map(g_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
    {
        memcpy(m.pData, &cb, sizeof(cb));
        g_context->Unmap(g_cbuffer, 0);
    }
}

// Lazily compile + allocate pipeline. Safe to call on every Present — fast
// path once g_pipelineReady is true.
static bool EnsurePipeline()
{
    if (g_pipelineReady) return true;
    if (!CompileAndCreateShaders()) return false;
    if (!CreateDeviceResources())   return false;
    g_pipelineReady = true;
    return true;
}

// Run the six-pass pipeline against the current back-buffer texture. The
// analysis + blur stages always sweep the full back-buffer (the histogram
// and the local-contrast neighbourhood are scene-wide), but the final
// tonemap is clipped per dirty rect — DWM only scans out the rectangles in
// `rects`, and a draw outside them never reaches the panel.
static bool RunPipeline(ID3D11Texture2D* backBuffer, const RECT* rects, int numRects,
                        MonitorTarget* tgt)
{
    if (!rects || numRects <= 0 || !tgt) return false;
    PerMonitor& pm = tgt->res;

    D3D11_TEXTURE2D_DESC bbd; backBuffer->GetDesc(&bbd);
    if (!EnsureMonitorResources(pm, bbd.Width, bbd.Height)) return false;

    // This screen's own frame cadence (each monitor presents independently).
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    double dt = (double)(now.QuadPart - pm.qpcLast.QuadPart) * 1000.0 / (double)g_qpcFreq.QuadPart;
    pm.qpcLast = now;
    if (dt > 1000.0 || dt <= 0.0) dt = 16.6; // sanity clamp across long stalls / detours

    UpdateCbuffer(bbd.Width, bbd.Height, tgt, (float)dt);

    // Snapshot back-buffer → sceneTex (read source for all subsequent passes).
    g_context->CopyResource(pm.sceneTex, backBuffer);

    // Cbuffer is the only persistent binding across all stages.
    g_context->VSSetConstantBuffers(0, 1, &g_cbuffer);
    g_context->PSSetConstantBuffers(0, 1, &g_cbuffer);
    g_context->CSSetConstantBuffers(0, 1, &g_cbuffer);

    // ---- CS_Clear (zero histogram) ----
    ID3D11UnorderedAccessView* uavs[]   = { pm.histUAV, pm.adaptUAV };
    g_context->CSSetUnorderedAccessViews(0, 2, uavs, NULL);
    g_context->CSSetShader(g_csClear, NULL, 0);
    g_context->Dispatch(1, 1, 1);

    // ---- CS_Analyze (histogram accumulate) ----
    ID3D11ShaderResourceView* csIn[] = { pm.sceneSRV };
    g_context->CSSetShaderResources(0, 1, csIn);
    g_context->CSSetShader(g_csAnalyze, NULL, 0);
    UINT stride = (g_knobs.AnalyzeStride >= 1) ? (UINT)g_knobs.AnalyzeStride : 1u;
    UINT step   = stride * 16u;
    UINT dx = (bbd.Width  + step - 1u) / step;
    UINT dy = (bbd.Height + step - 1u) / step;
    g_context->Dispatch(dx, dy, 1);

    // ---- CS_Adapt (reduce + temporal blend) ----
    g_context->CSSetShader(g_csAdapt, NULL, 0);
    g_context->Dispatch(1, 1, 1);

    // Unbind UAVs/SRVs from CS so we can read them from PS.
    ID3D11UnorderedAccessView* nullUav[2] = { NULL, NULL };
    g_context->CSSetUnorderedAccessViews(0, 2, nullUav, NULL);
    ID3D11ShaderResourceView* nullCsSrv[1] = { NULL };
    g_context->CSSetShaderResources(0, 1, nullCsSrv);

    // ---- Raster setup (shared across blur + tonemap) ----
    D3D11_VIEWPORT vp = { 0.f, 0.f, (float)bbd.Width, (float)bbd.Height, 0.f, 1.f };
    g_context->RSSetViewports(1, &vp);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->IASetInputLayout(NULL);
    g_context->VSSetShader(g_vsPost, NULL, 0);
    g_context->PSSetSamplers(0, 1, &g_sampler);

    // Force a full-frame scissor for the blur passes so the neighbourhood mean
    // (the lift's base/zone) is computed across the whole frame. Otherwise the
    // blur inherits whatever scissor DWM left bound at Present, which can clip it
    // to a stale or empty region — leaving the base black, so the lift vanishes
    // at LiftLocality 0. The tonemap re-scissors to each dirty rect below.
    g_context->RSSetState(g_rasterScissor);
    {
        D3D11_RECT fullScissor = { 0, 0, (LONG)bbd.Width, (LONG)bbd.Height };
        g_context->RSSetScissorRects(1, &fullScissor);
    }

    ID3D11ShaderResourceView* nullSrvs[5] = { NULL, NULL, NULL, NULL, NULL };

    // ---- PS_BlurH → LumaH ----
    g_context->PSSetShaderResources(0, 5, nullSrvs);
    g_context->PSSetShaderResources(0, 1, &pm.sceneSRV);
    g_context->OMSetRenderTargets(1, &pm.lumaHRTV, NULL);
    g_context->PSSetShader(g_psBlurH, NULL, 0);
    g_context->Draw(3, 0);

    // ---- PS_BlurV → LumaBlur ----
    g_context->OMSetRenderTargets(0, NULL, NULL);
    g_context->PSSetShaderResources(0, 5, nullSrvs);
    g_context->PSSetShaderResources(3, 1, &pm.lumaHSRV);
    g_context->OMSetRenderTargets(1, &pm.lumaBlurRTV, NULL);
    g_context->PSSetShader(g_psBlurV, NULL, 0);
    g_context->Draw(3, 0);

    // ---- PS_Tonemap → back-buffer (clipped per dirty rect via scissor) ----
    g_context->OMSetRenderTargets(0, NULL, NULL);
    g_context->PSSetShaderResources(0, 5, nullSrvs);
    ID3D11RenderTargetView* bbRTV = NULL;
    if (FAILED(g_device->CreateRenderTargetView(backBuffer, NULL, &bbRTV)) || !bbRTV) return false;
    ID3D11ShaderResourceView* tmIn[5] = { pm.sceneSRV, pm.histSRV, pm.adaptSRV, NULL, pm.lumaBlurSRV };
    g_context->PSSetShaderResources(0, 5, tmIn);
    g_context->OMSetRenderTargets(1, &bbRTV, NULL);
    g_context->PSSetShader(g_psTonemap, NULL, 0);
    g_context->RSSetState(g_rasterScissor);
    for (int i = 0; i < numRects; i++)
    {
        D3D11_RECT sr = { rects[i].left, rects[i].top, rects[i].right, rects[i].bottom };
        g_context->RSSetScissorRects(1, &sr);
        g_context->Draw(3, 0);
    }
    bbRTV->Release();

    g_context->RSSetState(NULL);
    g_context->OMSetRenderTargets(0, NULL, NULL);
    g_context->PSSetShaderResources(0, 5, nullSrvs);
    return true;
}

// ===========================================================================
// 25H2: back buffer comes via overlaySwapChain->vt[24]() -> result->vt[19]()
// -> QueryInterface(ID3D11Texture2D).
// ===========================================================================

static ID3D11Texture2D* GetBackBuffer_25H2(void* overlaySwapChain)
{
    __try
    {
        if (!overlaySwapChain) return NULL;
        void** vt = *(void***)overlaySwapChain;
        if (!vt) return NULL;
        typedef void* (__fastcall *VirtFunc)(void*);
        VirtFunc f1 = (VirtFunc)vt[24]; if (!f1) return NULL;
        void* r1 = f1(overlaySwapChain); if (!r1) return NULL;
        void** vt2 = *(void***)r1; if (!vt2) return NULL;
        VirtFunc f2 = (VirtFunc)vt2[19]; if (!f2) return NULL;
        void* r2 = f2(r1); if (!r2) return NULL;
        ID3D11Texture2D* tex = NULL;
        if (FAILED(((IUnknown*)r2)->QueryInterface(IID_ID3D11Texture2D, (void**)&tex)) || !tex) return NULL;
        return tex;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}

// ===========================================================================
// Apply stubs — return true when DVHDR engages for this context (gates the
// DirectFlip / OverlaysEnabled hooks). Real six-pass pipeline lands later.
// ===========================================================================

static bool ApplyDvhdr_SwapChain(void* ctx, IDXGISwapChain* swap, const RECT* rects, int numRects)
{
    try
    {
        ID3D11Device* dev = NULL;
        if (FAILED(swap->GetDevice(IID_ID3D11Device, (void**)&dev))) return false;
        if (dev != g_device) { TeardownDevice(); InitDeviceFromDevice(dev); }
        dev->Release();

        ID3D11Texture2D* bb = NULL;
        if (FAILED(swap->GetBuffer(0, IID_ID3D11Texture2D, (void**)&bb))) return false;
        D3D11_TEXTURE2D_DESC desc; bb->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) { bb->Release(); return false; }
        MonitorTarget* tgt = GetTargetForContext(ctx);
        if (!tgt) { bb->Release(); return false; }
        if (!EnsurePipeline()) { bb->Release(); return false; }

        bool ok = RunPipeline(bb, rects, numRects, tgt);
        bb->Release();
        return ok;
    }
    catch (...) { return false; }
}

static bool ApplyDvhdr_Texture(void* ctx, ID3D11Texture2D* bb, const RECT* rects, int numRects)
{
    try
    {
        ID3D11Device* dev = NULL;
        bb->GetDevice(&dev);
        if (!dev) return false;
        if (dev != g_device) { TeardownDevice(); InitDeviceFromDevice(dev); }
        dev->Release();

        D3D11_TEXTURE2D_DESC desc; bb->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
        MonitorTarget* tgt = GetTargetForContext(ctx);
        if (!tgt) return false;
        if (!EnsurePipeline()) return false;

        return RunPipeline(bb, rects, numRects, tgt);
    }
    catch (...) { return false; }
}

// ===========================================================================
// Hook signatures
// ===========================================================================

typedef struct rectVec { RECT* start; RECT* end; RECT* cap; } rectVec;

// Legacy Win10/Win11 Present
typedef long (COverlayContext_Present_t)(void*, void*, unsigned int, rectVec*, unsigned int, bool);
// 24H2+25H2 Present — one extra void* before the trailing bool, long long return
typedef long long (COverlayContext_Present_24h2_t)(void*, void*, unsigned int, rectVec*, int, void*, bool);

typedef bool (COverlayContext_IsCandidateDirectFlipCompatbile_t)(void*, void*, void*, void*, int, unsigned int, bool, bool);
typedef bool (COverlayContext_IsCandidateDirectFlipCompatbile_24h2_t)(void*, void*, void*, void*, unsigned int, bool);
typedef bool (COverlayContext_OverlaysEnabled_t)(void*);

static COverlayContext_Present_t*       COverlayContext_Present_orig             = NULL;
static COverlayContext_Present_t*       COverlayContext_Present_real_orig        = NULL;
static COverlayContext_Present_24h2_t*  COverlayContext_Present_orig_24h2        = NULL;
static COverlayContext_Present_24h2_t*  COverlayContext_Present_real_orig_24h2   = NULL;
static COverlayContext_IsCandidateDirectFlipCompatbile_t*       COverlayContext_IsCandidateDirectFlipCompatbile_orig      = NULL;
static COverlayContext_IsCandidateDirectFlipCompatbile_24h2_t*  COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2 = NULL;
static COverlayContext_OverlaysEnabled_t*                       COverlayContext_OverlaysEnabled_orig                       = NULL;

// ===========================================================================
// Hook bodies
// ===========================================================================

static long COverlayContext_Present_hook(void* self, void* overlaySwapChain, unsigned int a3,
                                         rectVec* rects, unsigned int a5, bool a6)
{
    if (_ReturnAddress() < (void*)COverlayContext_Present_real_orig)
    {
        bool hwProtected = isWindows11
            ? *((bool*)overlaySwapChain + IOverlaySwapChain_HardwareProtected_offset_w11)
            : *((bool*)overlaySwapChain + IOverlaySwapChain_HardwareProtected_offset);
        if (hwProtected)
        {
            UnsetActive(self);
        }
        else
        {
            IDXGISwapChain* swap;
            if (isWindows11)
            {
                int sub = *(int*)((unsigned char*)overlaySwapChain - 4);
                void* real = (unsigned char*)overlaySwapChain - sub - 0x1b0;
                swap = *(IDXGISwapChain**)((unsigned char*)real + IOverlaySwapChain_IDXGISwapChain_offset_w11);
            }
            else
            {
                swap = *(IDXGISwapChain**)((unsigned char*)overlaySwapChain + IOverlaySwapChain_IDXGISwapChain_offset);
            }
            int n = (int)(rects->end - rects->start);
            if (swap && ApplyDvhdr_SwapChain(self, swap, rects->start, n)) SetActive(self);
            else                                                           UnsetActive(self);
        }
    }
    return COverlayContext_Present_orig(self, overlaySwapChain, a3, rects, a5, a6);
}

static long long COverlayContext_Present_hook_24h2(void* self, void* overlaySwapChain, unsigned int a3,
                                                   rectVec* rects, int a5, void* a6, bool a7)
{
    if (_ReturnAddress() < (void*)COverlayContext_Present_real_orig_24h2 || isWindows11_24h2 || isWindows11_25h2)
    {
        int n = (int)(rects->end - rects->start);
        if (isWindows11_25h2)
        {
            ID3D11Texture2D* bb = GetBackBuffer_25H2(overlaySwapChain);
            if (bb)
            {
                if (ApplyDvhdr_Texture(self, bb, rects->start, n)) SetActive(self);
                else                                               UnsetActive(self);
                bb->Release();
            }
            else
            {
                UnsetActive(self);
            }
        }
        else // 24H2
        {
            bool hwProtected = *((bool*)overlaySwapChain + IOverlaySwapChain_HardwareProtected_offset_w11_24h2);
            if (hwProtected)
            {
                UnsetActive(self);
            }
            else
            {
                IDXGISwapChain* swap = *(IDXGISwapChain**)((unsigned char*)overlaySwapChain
                                       + IOverlaySwapChain_IDXGISwapChain_offset_w11_24h2);
                if (swap && ApplyDvhdr_SwapChain(self, swap, rects->start, n)) SetActive(self);
                else                                                           UnsetActive(self);
            }
        }
    }
    return COverlayContext_Present_orig_24h2(self, overlaySwapChain, a3, rects, a5, a6, a7);
}

// These three hooks force DWM composition (no DirectFlip / hardware overlay) so
// our Present hook can run the pipeline. Gate per-context on the targeted screens
// only — an untargeted monitor keeps its overlays/MPO untouched.
static bool COverlayContext_IsCandidateDirectFlipCompatbile_hook(void* self, void* a2, void* a3, void* a4,
                                                                 int a5, unsigned int a6, bool a7, bool a8)
{
    if (GetTargetForContext(self)) return false;
    return COverlayContext_IsCandidateDirectFlipCompatbile_orig(self, a2, a3, a4, a5, a6, a7, a8);
}

static bool COverlayContext_IsCandidateDirectFlipCompatbile_hook_24h2(void* self, void* a2, void* a3, void* a4,
                                                                      unsigned int a5, bool a6)
{
    if (GetTargetForContext(self)) return false;
    return COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2(self, a2, a3, a4, a5, a6);
}

static bool COverlayContext_OverlaysEnabled_hook(void* self)
{
    if (GetTargetForContext(self)) return false;
    return COverlayContext_OverlaysEnabled_orig(self);
}

// ===========================================================================
// Multi-version AOB scan
// ===========================================================================

static bool ScanAndHook(HMODULE dwmcore, const MODULEINFO& mi)
{
    auto base = (unsigned char*)dwmcore;
    auto sz   = (size_t)mi.SizeOfImage;

    if (isWindows11_25h2)
    {
        for (size_t i = 0; i + sizeof(COverlayContext_OverlaysEnabled_bytes_w11_25h2) <= sz; i++)
        {
            auto a = base + i;
            if (!COverlayContext_Present_orig_24h2
                && i + sizeof(COverlayContext_Present_bytes_w11_25h2) <= sz
                && !aob_match_inverse(a, COverlayContext_Present_bytes_w11_25h2, sizeof(COverlayContext_Present_bytes_w11_25h2)))
            {
                COverlayContext_Present_orig_24h2 = (COverlayContext_Present_24h2_t*)a;
                COverlayContext_Present_real_orig_24h2 = COverlayContext_Present_orig_24h2;
            }
            else if (!COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2
                && i + sizeof(COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_25h2) <= sz
                && !aob_match_inverse(a, COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_25h2, sizeof(COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_25h2)))
            {
                COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2 = (COverlayContext_IsCandidateDirectFlipCompatbile_24h2_t*)a;
            }
            else if (!COverlayContext_OverlaysEnabled_orig
                && i + sizeof(COverlayContext_OverlaysEnabled_bytes_w11_25h2) <= sz
                && !aob_match_inverse(a, COverlayContext_OverlaysEnabled_bytes_w11_25h2, sizeof(COverlayContext_OverlaysEnabled_bytes_w11_25h2)))
            {
                COverlayContext_OverlaysEnabled_orig = (COverlayContext_OverlaysEnabled_t*)a;
                // 25H2 instruction is `83 3D <rel32> 05` (cmp dword [rip+rel32], 5).
                // Global var = next-instruction + rel32 = a + 7 + rel32.
                int rel = *(int*)(a + 2);
                g_pOverlayTestMode = (int*)(a + 7 + rel);
            }
            if (COverlayContext_Present_orig_24h2
                && COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2
                && COverlayContext_OverlaysEnabled_orig) break;
        }
    }
    else if (isWindows11_24h2)
    {
        for (size_t i = 0; i + sizeof(COverlayContext_OverlaysEnabled_bytes_relative_w11_24h2) <= sz; i++)
        {
            auto a = base + i;
            if (!COverlayContext_Present_orig_24h2
                && i + sizeof(COverlayContext_Present_bytes_w11_24h2) <= sz
                && !aob_match_inverse(a, COverlayContext_Present_bytes_w11_24h2, sizeof(COverlayContext_Present_bytes_w11_24h2)))
            {
                COverlayContext_Present_orig_24h2 = (COverlayContext_Present_24h2_t*)a;
                COverlayContext_Present_real_orig_24h2 = COverlayContext_Present_orig_24h2;
            }
            else if (!COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2
                && i + sizeof(COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_24h2) <= sz
                && !aob_match_inverse(a, COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_24h2, sizeof(COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11_24h2)))
            {
                COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2 = (COverlayContext_IsCandidateDirectFlipCompatbile_24h2_t*)a;
            }
            else if (!COverlayContext_OverlaysEnabled_orig
                && i + sizeof(COverlayContext_OverlaysEnabled_bytes_relative_w11_24h2) <= sz
                && !aob_match_inverse(a, COverlayContext_OverlaysEnabled_bytes_relative_w11_24h2, sizeof(COverlayContext_OverlaysEnabled_bytes_relative_w11_24h2)))
            {
                // The pattern is a `call rel32` site; resolve to the target.
                COverlayContext_OverlaysEnabled_orig = (COverlayContext_OverlaysEnabled_t*)get_relative_address(a, 1, 5);
            }
            if (COverlayContext_Present_orig_24h2
                && COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2
                && COverlayContext_OverlaysEnabled_orig) break;
        }
    }
    else if (isWindows11)
    {
        for (size_t i = 0; i + sizeof(COverlayContext_OverlaysEnabled_bytes_w11) <= sz; i++)
        {
            auto a = base + i;
            if (!COverlayContext_Present_orig
                && i + sizeof(COverlayContext_Present_bytes_w11) <= sz
                && !aob_match_inverse(a, COverlayContext_Present_bytes_w11, sizeof(COverlayContext_Present_bytes_w11)))
            {
                COverlayContext_Present_orig = (COverlayContext_Present_t*)a;
                COverlayContext_Present_real_orig = COverlayContext_Present_orig;
            }
            else if (!COverlayContext_IsCandidateDirectFlipCompatbile_orig
                && i + sizeof(COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11) <= sz
                && !aob_match_inverse(a, COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11, sizeof(COverlayContext_IsCandidateDirectFlipCompatbile_bytes_w11)))
            {
                COverlayContext_IsCandidateDirectFlipCompatbile_orig = (COverlayContext_IsCandidateDirectFlipCompatbile_t*)a;
            }
            else if (!COverlayContext_OverlaysEnabled_orig
                && i + sizeof(COverlayContext_OverlaysEnabled_bytes_w11) <= sz
                && !aob_match_inverse(a, COverlayContext_OverlaysEnabled_bytes_w11, sizeof(COverlayContext_OverlaysEnabled_bytes_w11)))
            {
                COverlayContext_OverlaysEnabled_orig = (COverlayContext_OverlaysEnabled_t*)a;
            }
            if (COverlayContext_Present_orig
                && COverlayContext_IsCandidateDirectFlipCompatbile_orig
                && COverlayContext_OverlaysEnabled_orig) break;
        }
    }
    else // Win10
    {
        for (size_t i = 0; i + sizeof(COverlayContext_Present_bytes) <= sz; i++)
        {
            auto a = base + i;
            if (!COverlayContext_Present_orig
                && !memcmp(a, COverlayContext_Present_bytes, sizeof(COverlayContext_Present_bytes)))
            {
                COverlayContext_Present_orig = (COverlayContext_Present_t*)a;
                COverlayContext_Present_real_orig = COverlayContext_Present_orig;
            }
            else if (!COverlayContext_IsCandidateDirectFlipCompatbile_orig
                && !memcmp(a, COverlayContext_IsCandidateDirectFlipCompatbile_bytes, sizeof(COverlayContext_IsCandidateDirectFlipCompatbile_bytes)))
            {
                static int found = 0;
                if (++found == 2)
                    COverlayContext_IsCandidateDirectFlipCompatbile_orig = (COverlayContext_IsCandidateDirectFlipCompatbile_t*)(a - 0xa);
            }
            else if (!COverlayContext_OverlaysEnabled_orig
                && !memcmp(a, COverlayContext_OverlaysEnabled_bytes, sizeof(COverlayContext_OverlaysEnabled_bytes)))
            {
                COverlayContext_OverlaysEnabled_orig = (COverlayContext_OverlaysEnabled_t*)(a - 0x7);
            }
            if (COverlayContext_Present_orig
                && COverlayContext_IsCandidateDirectFlipCompatbile_orig
                && COverlayContext_OverlaysEnabled_orig) break;
        }
    }

    // Validate per-version
    bool legacy_ok = COverlayContext_Present_orig
                  && COverlayContext_IsCandidateDirectFlipCompatbile_orig
                  && COverlayContext_OverlaysEnabled_orig;
    bool modern_ok = COverlayContext_Present_orig_24h2
                  && COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2
                  && COverlayContext_OverlaysEnabled_orig;
    if (!legacy_ok && !modern_ok) return false;

    MH_Initialize();

    if (isWindows11_24h2 || isWindows11_25h2)
    {
        MH_CreateHook((PVOID)COverlayContext_Present_orig_24h2,
                      (PVOID)COverlayContext_Present_hook_24h2,
                      (PVOID*)&COverlayContext_Present_orig_24h2);
        MH_CreateHook((PVOID)COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2,
                      (PVOID)COverlayContext_IsCandidateDirectFlipCompatbile_hook_24h2,
                      (PVOID*)&COverlayContext_IsCandidateDirectFlipCompatbile_orig_24h2);
    }
    else
    {
        MH_CreateHook((PVOID)COverlayContext_Present_orig,
                      (PVOID)COverlayContext_Present_hook,
                      (PVOID*)&COverlayContext_Present_orig);
        MH_CreateHook((PVOID)COverlayContext_IsCandidateDirectFlipCompatbile_orig,
                      (PVOID)COverlayContext_IsCandidateDirectFlipCompatbile_hook,
                      (PVOID*)&COverlayContext_IsCandidateDirectFlipCompatbile_orig);
    }
    MH_CreateHook((PVOID)COverlayContext_OverlaysEnabled_orig,
                  (PVOID)COverlayContext_OverlaysEnabled_hook,
                  (PVOID*)&COverlayContext_OverlaysEnabled_orig);
    MH_EnableHook(MH_ALL_HOOKS);
    return true;
}

// ===========================================================================
// DllMain
// ===========================================================================

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        HMODULE dwmcore = GetModuleHandleW(L"dwmcore.dll");
        if (!dwmcore) return FALSE;

        MODULEINFO mi = {};
        if (!GetModuleInformation(GetCurrentProcess(), dwmcore, &mi, sizeof(mi))) return FALSE;

        // 3-way version detection — most specific first.
        OSVERSIONINFOEXW v25 = { sizeof(OSVERSIONINFOEXW) }; v25.dwBuildNumber = 26200;
        OSVERSIONINFOEXW v24 = { sizeof(OSVERSIONINFOEXW) }; v24.dwBuildNumber = 26100;
        OSVERSIONINFOEXW v11 = { sizeof(OSVERSIONINFOEXW) }; v11.dwBuildNumber = 22000;
        ULONGLONG mask = 0;
        VER_SET_CONDITION(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
        if (VerifyVersionInfoW(&v25, VER_BUILDNUMBER, mask))      isWindows11_25h2 = true;
        else if (VerifyVersionInfoW(&v24, VER_BUILDNUMBER, mask)) isWindows11_24h2 = true;
        else if (VerifyVersionInfoW(&v11, VER_BUILDNUMBER, mask)) isWindows11      = true;

        LoadTargetsFromRegistry();
        LoadKnobsFromIni();
        ResolvePerMonitorCaps();

        if (!ScanAndHook(dwmcore, mi)) return FALSE;

        // 25H2: disable DirectFlip/MPO process-wide by flipping the internal
        // OverlayTestMode flag to 5 (matches the reference's behaviour).
        if (g_pOverlayTestMode) *g_pOverlayTestMode = 5;
        break;
    }
    case DLL_PROCESS_DETACH:
        if (g_pOverlayTestMode) { *g_pOverlayTestMode = 0; }
        MH_Uninitialize();
        Sleep(100);
        TeardownDevice();
        break;
    }
    return TRUE;
}
