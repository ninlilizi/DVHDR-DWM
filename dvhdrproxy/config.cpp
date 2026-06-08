#include "config.h"

DvhdrKnobs g_knobs;

static HMODULE g_self = NULL;
static char    g_iniPath[MAX_PATH] = {};

static const UINT CSP_SCRGB = 1u;
static const UINT CSP_HDR10 = 2u;

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
}

UINT Config_ColorSpaceForFormat(DXGI_FORMAT fmt)
{
    if (g_knobs.ColorSpace == 1) return CSP_SCRGB;
    if (g_knobs.ColorSpace == 2) return CSP_HDR10;

    switch (fmt)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return CSP_SCRGB;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return CSP_HDR10;
    default:
        return 0; // not an HDR back buffer — pass through
    }
}

void Config_FillCbuffer(DvhdrCbGpu* cb, UINT w, UINT h, UINT colorSpace, float frameTimeMs)
{
    cb->BufferW             = w;
    cb->BufferH             = h;
    cb->ColorSpace          = colorSpace;
    cb->FrameTimeMs         = frameTimeMs;
    cb->DisplayPeak         = g_knobs.DisplayPeak;
    cb->DisplayMaxFALL      = g_knobs.DisplayMaxFALL;
    cb->DisplayBlack        = g_knobs.DisplayBlack;
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
    cb->BlackLift           = g_knobs.BlackLift;
    cb->ShadowToe           = g_knobs.ShadowToe;
    cb->ChromaCorrect       = g_knobs.ChromaCorrect;
    cb->LiftLocality        = g_knobs.LiftLocality;
    cb->DebandThreshold     = g_knobs.DebandThreshold;
    cb->DebandRange         = g_knobs.DebandRange;
}
