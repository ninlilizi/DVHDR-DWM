#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <shellapi.h>  // Shell_NotifyIcon - guard tray status icon
#include <dwmapi.h>    // DWMWA_CLOAKED - game-guard foreground test
#include <wmistr.h>    // WNODE_HEADER for EVENT_TRACE_PROPERTIES
#include <evntrace.h>  // ETW session control - present-rate game detection
#include <evntcons.h>  // ETW real-time consumption (EVENT_RECORD)
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
