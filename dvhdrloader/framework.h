#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <shellapi.h>
#include <dwmapi.h>    // DWMWA_CLOAKED — skip suspended UWP ghost windows
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>   // IDXGIOutput5::DuplicateOutput1 — HDR/WCG desktop capture
#include <wmistr.h>    // WNODE_HEADER for EVENT_TRACE_PROPERTIES
#include <evntrace.h>  // ETW session control - present-rate game detection
#include <evntcons.h>  // ETW real-time consumption (EVENT_RECORD)
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
