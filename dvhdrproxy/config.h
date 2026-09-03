#pragma once
#include "framework.h"

static const UINT CSP_SCRGB = 1u;
static const UINT CSP_HDR10 = 2u;
static const UINT CSP_SDR   = 3u;

// GPU-side cbuffer mirror - layout must match `cbuffer DVHDRCb` in
// dvhdr_dwm.hlsl (4-float rows, 16-byte aligned). Identical to the DWM payload.
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

    float SdrWhiteNits;
    float SdrGamma;
    float SdrSteps;
    UINT  PreserveAlpha;
};
static_assert(sizeof(DvhdrCbGpu) == 144, "cbuffer layout drift");

struct DvhdrKnobs
{
    int   ColorSpace;             // 0 auto / 1 scRGB / 2 HDR10 / 3 SDR
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
    float SdrWhiteNits;           // 0 = auto (OS SDR white level while the display is in HDR mode)
    float SdrFallbackWhiteNits;   // used when auto cannot resolve one
    float SdrGamma;               // 0 = auto, -1 = sRGB piecewise, > 0 = power-law exponent
    int   ProcessSDR;             // 0 = pass SDR chains through untouched
    int   LogEnabled;             // [Debug] Log
};

extern DvhdrKnobs g_knobs;

// What the pass needs to know about the surface it is about to rewrite.
struct SurfaceInfo
{
    UINT  ColorSpace;     // CSP_SCRGB / CSP_HDR10 / CSP_SDR
    float SdrWhiteNits;   // CSP_SDR only: luminance of code 1.0
    float SdrGamma;       // CSP_SDR only: 0 = sRGB piecewise, else the exponent
    float SdrSteps;       // CSP_SDR only: code steps of the back buffer
};

// Records the module handle of this proxy DLL so config can be read from the
// directory it was dropped into (next to the game executable), ReShade-style.
void Config_SetSelfModule(HMODULE self);

// Read dvhdr.ini from beside the proxy DLL. Safe to call repeatedly.
void Config_Load();

// Decide how to treat a swap chain's back buffer: the [Source] override first,
// then the colour space the game declared through SetColorSpace1, then the
// format (10-bit UNORM reads as HDR10 only while the display is in HDR mode).
// SDR surfaces also pick up their white level and transfer curve here. Returns
// false when the surface is one the pass does not handle - caller passes through,
// and *why (optional) names the reason for the log.
bool Config_ClassifySurface(IDXGISwapChain* sc, DXGI_FORMAT fmt, SurfaceInfo* out, const char** why = NULL);

// Short human-readable summary of a classified surface, for the log.
void Config_DescribeSurface(const SurfaceInfo& surf, char* buf, size_t n);

// Fill a cbuffer snapshot from the current knobs + surface + measured frame time + size.
void Config_FillCbuffer(DvhdrCbGpu* out, UINT w, UINT h, const SurfaceInfo& surf, float frameTimeMs);
