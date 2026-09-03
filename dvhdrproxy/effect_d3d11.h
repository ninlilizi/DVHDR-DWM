#pragma once
#include "framework.h"

// Run the DVHDR tonemap over the swap chain's current back buffer. Returns
// false (caller presents unmodified) when the swap chain is not D3D11, the
// surface is not one the pass handles, or any resource step fails. State is
// kept per swap chain; the caller serialises these calls. `present` carries
// Present1's parameters (NULL for Present): with dirty rectangles only those
// regions of the back buffer are fresh.
bool Effect11_Apply(IDXGISwapChain* swap, const DXGI_PRESENT_PARAMETERS* present);

// Drop that chain's size-bound resources so they rebuild at the next Apply
// (ResizeBuffers).
void Effect11_OnResize(IDXGISwapChain* swap);

void Effect11_Shutdown();
