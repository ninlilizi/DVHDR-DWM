#pragma once
#include "framework.h"

// Record + submit the DVHDR tonemap over the swap chain's current back buffer
// using D3D12. Requires a command queue captured from the game's
// ExecuteCommandLists on the chain's device (see Effect12_SetQueue). Returns
// false (caller presents unmodified) when the swap chain is not D3D12, no queue
// is known for its device yet, the surface is not one the pass handles, or any
// step fails. State is kept per swap chain; the caller serialises these calls.
// `present` carries Present1's parameters (NULL for Present): with dirty
// rectangles only those regions of the back buffer are fresh.
bool Effect12_Apply(IDXGISwapChain* swap, const DXGI_PRESENT_PARAMETERS* present);

// Remember the DIRECT queue a device presents through. Called from the
// ID3D12CommandQueue::ExecuteCommandLists hook, from any thread.
void Effect12_SetQueue(ID3D12CommandQueue* queue);

void Effect12_OnResize(IDXGISwapChain* swap);
void Effect12_Shutdown();
