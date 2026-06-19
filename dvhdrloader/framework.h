#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>   // IDXGIOutput5::DuplicateOutput1 — HDR/WCG desktop capture
#include <cstdio>
#include <string>
#include <vector>
