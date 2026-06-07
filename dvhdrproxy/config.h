#pragma once
#include "framework.h"

// GPU-side cbuffer mirror — layout must match `cbuffer DVHDRCb` in
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
    float _pad1, _pad2;
};
static_assert(sizeof(DvhdrCbGpu) == 128, "cbuffer layout drift");

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
};

extern DvhdrKnobs g_knobs;

// Records the module handle of this proxy DLL so config can be read from the
// directory it was dropped into (next to the game executable), ReShade-style.
void Config_SetSelfModule(HMODULE self);

// Read dvhdr.ini from beside the proxy DLL. Safe to call repeatedly.
void Config_Load();

// Map a back-buffer DXGI format to a shader ColorSpace (CSP_SCRGB=1, CSP_HDR10=2),
// honouring the [Source] ColorSpace override. Returns 0 when the format is not an
// HDR format we handle and no override forces one — caller should pass through.
UINT Config_ColorSpaceForFormat(DXGI_FORMAT fmt);

// Fill a cbuffer snapshot from the current knobs + measured frame time + size.
void Config_FillCbuffer(DvhdrCbGpu* out, UINT w, UINT h, UINT colorSpace, float frameTimeMs);
