#pragma once
#include "framework.h"

// Diagnostic log, enabled by [Debug] Log = 1. Written to
// %TEMP%\dvhdrproxy-<exe name>.log, one file per process start.
void Log_Init(bool enabled);
void Log_Shutdown();
bool Log_Enabled();
void Log_Write(const char* fmt, ...);

// One line per swap chain whenever its profile (api, device, format, size,
// verdict) changes, plus a description of the chain the first time it is seen.
void Log_Chain(IDXGISwapChain* sc, const char* api, const void* device,
               DXGI_FORMAT fmt, UINT w, UINT h, const char* verdict);
