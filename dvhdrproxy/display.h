#pragma once
#include "framework.h"

// Remember the colour space a game declared through SetColorSpace1, on the swap
// chain itself (DXGI private data), so classification can read it back at Present.
void Display_RecordColorSpace(IDXGISwapChain* sc, DXGI_COLOR_SPACE_TYPE cs);
void Display_ClearRecordedColorSpace(IDXGISwapChain* sc);
bool Display_GetRecordedColorSpace(IDXGISwapChain* sc, DXGI_COLOR_SPACE_TYPE* out);

// Likewise for the HDR10 mastering metadata a game declares through
// SetHDRMetaData: its content peak and frame-average light levels, in nits.
void Display_RecordHdrMetadata(IDXGISwapChain* sc, DXGI_HDR_METADATA_TYPE type, UINT size, const void* data);
bool Display_GetRecordedHdrMetadata(IDXGISwapChain* sc, float* peakNits, float* fallNits);

// False for chains composed with alpha (premultiplied / straight): overlays,
// which the pass leaves alone.
bool Display_IsOpaqueChain(IDXGISwapChain* sc);

struct DisplayState
{
    bool  Known;          // false when the output could not be resolved
    bool  HdrMode;        // the output is scanning out in HDR (PQ) mode
    float SdrWhiteNits;   // Windows' SDR white level for that output; 0 when unavailable
    int   DisplayNumber;  // the Windows display number of that output (the N of [Display.N]); 0 = unknown
};

// State of the output the swap chain presents to. Cached about a second per chain.
DisplayState Display_Query(IDXGISwapChain* sc);
