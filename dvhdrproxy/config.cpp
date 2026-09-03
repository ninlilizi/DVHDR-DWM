#include "config.h"
#include "display.h"

DvhdrKnobs g_knobs;

static HMODULE g_self = NULL;
static char    g_iniPath[MAX_PATH] = {};

void Config_SetSelfModule(HMODULE self)
{
    g_self = self;
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(g_self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { g_iniPath[0] = '\0'; return; }
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    snprintf(g_iniPath, sizeof(g_iniPath), "%sdvhdr.ini", path);
}

static float IniFloat(const char* sec, const char* key, float defVal)
{
    char buf[64];
    GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), g_iniPath);
    if (!buf[0]) return defVal;
    return (float)atof(buf);
}

// [SDR] Gamma accepts "auto", "sRGB", or a power-law exponent.
static float IniSdrGamma()
{
    char buf[64];
    GetPrivateProfileStringA("SDR", "Gamma", "", buf, sizeof(buf), g_iniPath);
    if (buf[0] == 's' || buf[0] == 'S') return -1.0f;
    return (float)atof(buf);   // blank and "auto" both parse to 0
}

void Config_Load()
{
    g_knobs.ColorSpace          = GetPrivateProfileIntA("Source",      "ColorSpace",          0,       g_iniPath);
    g_knobs.DisplayPeak         = IniFloat("Display",   "Peak",                 1300.0f);
    g_knobs.DisplayMaxFALL      = IniFloat("Display",   "MaxFALL",              265.0f);
    g_knobs.DisplayBlack        = IniFloat("Display",   "Black",                0.0f);
    g_knobs.BlackLift           = IniFloat("Display",   "BlackLift",            0.00248f);
    g_knobs.HeadroomPercent     = IniFloat("Governor",  "HeadroomPercent",      90.0f);
    g_knobs.MinGain             = IniFloat("Governor",  "MinGain",              0.25f);
    g_knobs.LiftStrength        = IniFloat("Governor",  "LiftStrength",         0.25f);
    g_knobs.MaxGain             = IniFloat("Governor",  "MaxGain",              1.5f);
    g_knobs.HighlightProtect    = IniFloat("Governor",  "HighlightProtect",     80.0f);
    g_knobs.ShadowToe           = IniFloat("Governor",  "ShadowToe",            0.25f);
    g_knobs.PeakPercentile      = IniFloat("Governor",  "PeakPercentile",       99.7f);
    g_knobs.AttackMs            = IniFloat("Temporal",  "AttackMs",             80.0f);
    g_knobs.ReleaseMs           = IniFloat("Temporal",  "ReleaseMs",            600.0f);
    g_knobs.DynamicContrast     = IniFloat("ToneCurve", "DynamicContrast",      0.25f);
    g_knobs.DetailGain          = IniFloat("ToneCurve", "DetailGain",           1.0f);
    g_knobs.DetailRadius        = IniFloat("ToneCurve", "DetailRadius",         12.0f);
    g_knobs.DetailBias          = IniFloat("ToneCurve", "DetailBias",           0.0f);
    g_knobs.UseHighlightRolloff = GetPrivateProfileIntA("ToneCurve", "UseHighlightRolloff", 1,       g_iniPath);
    g_knobs.Strength            = IniFloat("ToneCurve", "Strength",             1.0f);
    g_knobs.AnalyzeStride       = GetPrivateProfileIntA("Performance","AnalyzeStride",        2,       g_iniPath);
    g_knobs.DebugOverlay        = GetPrivateProfileIntA("Debug",      "Overlay",              0,       g_iniPath);
    g_knobs.DitherStrength      = IniFloat("Dither",    "Strength",             1.5f);
    g_knobs.DitherActivity      = IniFloat("Dither",    "Activity",             0.002f);
    g_knobs.DitherFloor         = IniFloat("Dither",    "Floor",                0.4f);
    g_knobs.ChromaCorrect       = IniFloat("Color",     "ChromaCorrect",        1.0f);
    g_knobs.LiftLocality        = IniFloat("ToneCurve", "LiftLocality",         0.0f);
    g_knobs.DebandThreshold     = IniFloat("Deband",    "Threshold",            3.0f);
    g_knobs.DebandRange         = IniFloat("Deband",    "Range",                16.0f);
    g_knobs.SdrWhiteNits         = IniFloat("SDR",      "WhiteNits",            0.0f);
    g_knobs.SdrFallbackWhiteNits = IniFloat("SDR",      "FallbackWhiteNits",    200.0f);
    g_knobs.SdrGamma             = IniSdrGamma();
    g_knobs.ProcessSDR           = GetPrivateProfileIntA("Source", "ProcessSDR", 1, g_iniPath);
    g_knobs.LogEnabled           = GetPrivateProfileIntA("Debug",  "Log",        0, g_iniPath);
    g_knobs.SceneCut             = IniFloat("Temporal",  "SceneCut",      0.06f);
    g_knobs.Deadband             = IniFloat("Temporal",  "Deadband",      2.0f) * 0.01f;
    g_knobs.AblWindowS           = IniFloat("Governor",  "AblWindow",     4.0f);
    g_knobs.FastCeiling          = IniFloat("Governor",  "FastCeiling",   150.0f) * 0.01f;
    g_knobs.LiftMetric           = GetPrivateProfileIntA("Governor",    "LiftMetric",    1, g_iniPath);
    g_knobs.BaseEdgeSigma        = IniFloat("ToneCurve", "BaseEdgeSigma", 0.08f);
    g_knobs.BaseDownscale        = GetPrivateProfileIntA("Performance", "BaseDownscale", 2, g_iniPath);
    g_knobs.ShadowDesat          = IniFloat("Color",     "ShadowDesat",   1.0f);
    g_knobs.GamutClip            = GetPrivateProfileIntA("Color",       "GamutClip",     1, g_iniPath);
    g_knobs.DitherTemporal       = GetPrivateProfileIntA("Dither",      "Temporal",      1, g_iniPath);
    g_knobs.DitherShape          = GetPrivateProfileIntA("Dither",      "Shape",         1, g_iniPath);
    g_knobs.DitherWideSpan       = IniFloat("Dither",    "WideSpan",      12.0f);
    g_knobs.DitherWideActivity   = IniFloat("Dither",    "WideActivity",  0.0015f);
    g_knobs.DitherHighlightFrom  = IniFloat("Dither",    "HighlightFrom", 200.0f);
    g_knobs.DitherHighlightBoost = IniFloat("Dither",    "HighlightBoost", 2.0f);
}

// Panel capabilities per monitor: a [Display.N] section may override any of
// Peak / MaxFALL / Black / BlackLift for Windows display N, as on the DWM side;
// anything omitted there inherits the global [Display]. Read once per monitor.
struct MonitorCaps
{
    int   number = 0;
    float Peak = 0, MaxFALL = 0, Black = 0, BlackLift = 0;
};
static const int   kMonitorCaps = 8;
static MonitorCaps g_monitorCaps[kMonitorCaps];

static void ResolveCaps(int displayNumber, SurfaceInfo* out)
{
    out->Peak      = g_knobs.DisplayPeak;
    out->MaxFALL   = g_knobs.DisplayMaxFALL;
    out->Black     = g_knobs.DisplayBlack;
    out->BlackLift = g_knobs.BlackLift;
    if (displayNumber <= 0) return;

    MonitorCaps* c = NULL;
    for (int i = 0; i < kMonitorCaps && !c; i++)
        if (g_monitorCaps[i].number == displayNumber) c = &g_monitorCaps[i];
    if (!c)
    {
        for (int i = 0; i < kMonitorCaps && !c; i++)
            if (g_monitorCaps[i].number == 0) c = &g_monitorCaps[i];
        if (!c) c = &g_monitorCaps[0];
        char sec[32];
        snprintf(sec, sizeof(sec), "Display.%d", displayNumber);
        c->number    = displayNumber;
        c->Peak      = IniFloat(sec, "Peak",      g_knobs.DisplayPeak);
        c->MaxFALL   = IniFloat(sec, "MaxFALL",   g_knobs.DisplayMaxFALL);
        c->Black     = IniFloat(sec, "Black",     g_knobs.DisplayBlack);
        c->BlackLift = IniFloat(sec, "BlackLift", g_knobs.BlackLift);
    }
    out->Peak      = c->Peak;
    out->MaxFALL   = c->MaxFALL;
    out->Black     = c->Black;
    out->BlackLift = c->BlackLift;
}

static bool IsSrgbFormat(DXGI_FORMAT fmt)
{
    return fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
        || fmt == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

static bool IsEightBitFormat(DXGI_FORMAT fmt)
{
    return fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM
        || fmt == DXGI_FORMAT_B8G8R8X8_UNORM || IsSrgbFormat(fmt);
}

// A declaration the format cannot carry is a stale one, left behind when the
// buffers changed format without a fresh declaration: PQ never sits in 8 bits
// and scRGB only ever in FP16.
static UINT ColorSpaceFromDeclared(DXGI_COLOR_SPACE_TYPE cs, DXGI_FORMAT fmt)
{
    switch (cs)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return IsEightBitFormat(fmt) ? 0 : CSP_HDR10;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:    return (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? CSP_SCRGB : 0;
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:    return CSP_SDR;
    default:                                         return 0; // studio range / YCbCr / 2020 SDR: let the format decide
    }
}

// Reached only when the game never declared a colour space. DXGI's own default
// for every UNORM format, 10-bit included, is sRGB: an HDR10 game has to declare
// G2084 to get PQ out of the compositor at all, so an undeclared 10-bit buffer is
// an SDR one that merely wanted finer steps. FP16 defaults to scRGB.
static UINT ColorSpaceFromFormat(DXGI_FORMAT fmt)
{
    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) return CSP_SCRGB;
    if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM || IsEightBitFormat(fmt)) return CSP_SDR;
    return 0;
}

static float ResolveSdrWhite(const DisplayState& ds)
{
    if (g_knobs.SdrWhiteNits > 0.0f) return g_knobs.SdrWhiteNits;
    if (ds.HdrMode && ds.SdrWhiteNits > 0.0f) return ds.SdrWhiteNits;
    return (g_knobs.SdrFallbackWhiteNits > 0.0f) ? g_knobs.SdrFallbackWhiteNits : 200.0f;
}

// Windows composes an SDR chain onto an HDR display through the piecewise sRGB
// curve; a panel driven natively in SDR tracks a 2.2 power law. An _SRGB view
// decodes and re-encodes in hardware, so the shader sees linear light there.
static float ResolveSdrGamma(DXGI_FORMAT fmt, const DisplayState& ds)
{
    if (IsSrgbFormat(fmt))       return 1.0f;
    if (g_knobs.SdrGamma < 0.0f) return 0.0f;
    if (g_knobs.SdrGamma > 0.0f) return g_knobs.SdrGamma;
    return ds.HdrMode ? 0.0f : 2.2f;
}

// Video overlay chains carry YUV planes that the display hardware converts and
// blends itself; there is no RGB surface to tonemap.
static bool IsYuvFormat(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_NV12: case DXGI_FORMAT_P010: case DXGI_FORMAT_P016:
    case DXGI_FORMAT_420_OPAQUE: case DXGI_FORMAT_YUY2: case DXGI_FORMAT_AYUV:
    case DXGI_FORMAT_Y410: case DXGI_FORMAT_Y416: case DXGI_FORMAT_Y210: case DXGI_FORMAT_Y216:
    case DXGI_FORMAT_NV11: case DXGI_FORMAT_P208: case DXGI_FORMAT_V208: case DXGI_FORMAT_V408:
        return true;
    default:
        return false;
    }
}

static float StepsForFormat(DXGI_FORMAT fmt)
{
    return IsEightBitFormat(fmt) ? 255.0f : 1023.0f;
}

bool Config_ClassifySurface(IDXGISwapChain* sc, DXGI_FORMAT fmt, SurfaceInfo* out, const char** why)
{
    const char* dummy;
    if (!why) why = &dummy;
    *why = NULL;

    if (!Display_IsOpaqueChain(sc)) { *why = "refused: alpha-composed overlay chain"; return false; }

    UINT csp = 0;
    if (g_knobs.ColorSpace >= 1 && g_knobs.ColorSpace <= 3)
    {
        csp = (UINT)g_knobs.ColorSpace;
    }
    else
    {
        DXGI_COLOR_SPACE_TYPE declared;
        if (Display_GetRecordedColorSpace(sc, &declared))
            csp = ColorSpaceFromDeclared(declared, fmt);
        if (csp == 0) csp = ColorSpaceFromFormat(fmt);
    }
    if (csp == 0)
    {
        *why = IsYuvFormat(fmt) ? "refused: YUV video overlay chain, composed by the display hardware"
                                : "refused: format not handled";
        return false;
    }
    if (csp == CSP_SDR && !g_knobs.ProcessSDR) { *why = "refused: SDR processing disabled"; return false; }

    *out = {};
    out->ColorSpace = csp;

    // The monitor holding most of the window decides the panel capabilities,
    // so they follow the window from screen to screen.
    DisplayState ds = Display_Query(sc);
    out->DisplayNumber = ds.DisplayNumber;
    ResolveCaps(ds.DisplayNumber, out);

    // An scRGB chain on a monitor that is not in HDR mode is clipped by DWM at
    // 1.0, which is 80 nits: hold the ceiling there so the rolloff does the
    // compressing gracefully instead of the compositor chopping it.
    if (csp == CSP_SCRGB && ds.Known && !ds.HdrMode)
    {
        out->SdrOutput = true;
        if (out->Peak    > 80.0f) out->Peak    = 80.0f;
        if (out->MaxFALL > 80.0f) out->MaxFALL = 80.0f;
    }

    if (csp == CSP_SDR)
    {
        out->SdrWhiteNits = ResolveSdrWhite(ds);
        out->SdrGamma     = ResolveSdrGamma(fmt, ds);
        out->SdrSteps     = StepsForFormat(fmt);
    }

    float peak = 0.0f, fall = 0.0f;
    if (Display_GetRecordedHdrMetadata(sc, &peak, &fall)) out->ContentPeak = peak;
    return true;
}

void Config_DescribeSurface(const SurfaceInfo& surf, char* buf, size_t n)
{
    int len;
    if (surf.ColorSpace == CSP_SDR)
        len = snprintf(buf, n, "applied: SDR white=%.0f gamma=%.2f steps=%.0f", surf.SdrWhiteNits, surf.SdrGamma, surf.SdrSteps);
    else if (surf.ContentPeak > 0.0f)
        len = snprintf(buf, n, "applied: %s, content peak %.0f nits", (surf.ColorSpace == CSP_HDR10) ? "HDR10" : "scRGB", surf.ContentPeak);
    else
        len = snprintf(buf, n, "applied: %s", (surf.ColorSpace == CSP_HDR10) ? "HDR10" : "scRGB");
    if (len < 0 || (size_t)len >= n) return;
    snprintf(buf + len, n - len, ", display %d peak %.0f fall %.0f%s", surf.DisplayNumber, surf.Peak, surf.MaxFALL,
             surf.SdrOutput ? " (monitor in SDR mode: ceiling 80 nits)" : "");
}

void Config_FillCbuffer(DvhdrCbGpu* cb, UINT w, UINT h, const SurfaceInfo& surf, float frameTimeMs, UINT frameIndex)
{
    cb->BufferW             = w;
    cb->BufferH             = h;
    cb->ColorSpace          = surf.ColorSpace;
    cb->FrameTimeMs         = frameTimeMs;
    cb->DisplayPeak         = surf.Peak;
    cb->DisplayMaxFALL      = surf.MaxFALL;
    cb->DisplayBlack        = surf.Black;
    cb->HeadroomPercent     = g_knobs.HeadroomPercent;
    cb->MinGain             = g_knobs.MinGain;
    cb->LiftStrength        = g_knobs.LiftStrength;
    cb->MaxGain             = g_knobs.MaxGain;
    cb->HighlightProtect    = g_knobs.HighlightProtect;
    cb->PeakPercentile      = g_knobs.PeakPercentile;
    cb->AttackMs            = g_knobs.AttackMs;
    cb->ReleaseMs           = g_knobs.ReleaseMs;
    cb->DynamicContrast     = g_knobs.DynamicContrast;
    cb->DetailGain          = g_knobs.DetailGain;
    cb->DetailRadius        = g_knobs.DetailRadius;
    cb->DetailBias          = g_knobs.DetailBias;
    cb->UseHighlightRolloff = (g_knobs.UseHighlightRolloff != 0) ? 1u : 0u;
    cb->Strength            = g_knobs.Strength;
    cb->DebugOverlay        = (UINT)g_knobs.DebugOverlay;
    cb->AnalyzeStride       = (g_knobs.AnalyzeStride >= 1) ? (UINT)g_knobs.AnalyzeStride : 1u;
    cb->DitherActivity      = g_knobs.DitherActivity;
    cb->DitherStrength      = g_knobs.DitherStrength;
    cb->DitherFloor         = g_knobs.DitherFloor;
    cb->BlackLift           = surf.BlackLift;
    cb->ShadowToe           = g_knobs.ShadowToe;
    cb->ChromaCorrect       = g_knobs.ChromaCorrect;
    cb->LiftLocality        = g_knobs.LiftLocality;
    cb->DebandThreshold     = g_knobs.DebandThreshold;
    cb->DebandRange         = g_knobs.DebandRange;
    cb->SdrWhiteNits        = surf.SdrWhiteNits;
    cb->SdrGamma            = surf.SdrGamma;
    cb->SdrSteps            = surf.SdrSteps;
    cb->PreserveAlpha       = 1u;   // a game's own alpha passes through untouched
    cb->FrameIndex          = frameIndex;
    cb->SceneCut            = g_knobs.SceneCut;
    cb->Deadband            = g_knobs.Deadband;
    cb->AblWindowS          = g_knobs.AblWindowS;
    cb->FastCeiling         = g_knobs.FastCeiling;
    cb->LiftMetric          = (g_knobs.LiftMetric != 0) ? 1u : 0u;
    cb->BaseEdgeSigma       = g_knobs.BaseEdgeSigma;
    cb->BaseScale           = (g_knobs.BaseDownscale >= 1) ? (UINT)g_knobs.BaseDownscale : 1u;
    cb->ShadowDesat         = g_knobs.ShadowDesat;
    cb->GamutClip           = (g_knobs.GamutClip != 0) ? 1u : 0u;
    cb->DitherTemporal      = (g_knobs.DitherTemporal != 0) ? 1u : 0u;
    cb->ContentPeak         = surf.ContentPeak;
    cb->DitherShape         = (g_knobs.DitherShape != 0) ? 1u : 0u;
    cb->DitherWideSpan      = g_knobs.DitherWideSpan;
    cb->DitherWideActivity  = g_knobs.DitherWideActivity;
    cb->DitherHighlightFrom = g_knobs.DitherHighlightFrom;
    cb->DitherHighlightBoost = g_knobs.DitherHighlightBoost;
}
