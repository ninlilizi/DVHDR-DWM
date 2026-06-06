#pragma once
#include "framework.h"

// Record + submit the DVHDR tonemap over the swap chain's current back buffer
// using D3D12. Requires a command queue captured from the game's
// ExecuteCommandLists (see Effect12_SetQueue). Returns false (caller presents
// unmodified) when the swap chain is not D3D12, no queue is known yet, the back
// buffer is not an HDR format we handle, or any step fails.
bool Effect12_Apply(IDXGISwapChain* swap);

// Remember the DIRECT queue the game presents through. Called from the
// ID3D12CommandQueue::ExecuteCommandLists hook.
void Effect12_SetQueue(ID3D12CommandQueue* queue);

void Effect12_OnResize();
void Effect12_Shutdown();
