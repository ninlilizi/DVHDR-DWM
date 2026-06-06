#pragma once
#include "framework.h"

// Run the DVHDR tonemap over the swap chain's current back buffer. Returns
// false (caller presents unmodified) when the swap chain is not D3D11, the
// back buffer is not an HDR format we handle, or any resource step fails.
bool Effect11_Apply(IDXGISwapChain* swap);

// Drop size-bound resources so they rebuild at the next Apply (ResizeBuffers).
void Effect11_OnResize();

void Effect11_Shutdown();
