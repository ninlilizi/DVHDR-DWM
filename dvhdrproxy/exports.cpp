// exports.cpp — resolves the genuine system dxgi.dll and fills the forwarder
// pointer slots (forwarders.asm) so every proxy export tail-jumps to the real
// implementation. Called once from DllMain before any forwarded export can fire.

#include "framework.h"

// The pointer slots are owned (defined) by forwarders.asm; declare them here.
extern "C" {
    extern void* p_ApplyCompatResolutionQuirking;
    extern void* p_CompatString;
    extern void* p_CompatValue;
    extern void* p_DXGIDumpJournal;
    extern void* p_PIXBeginCapture;
    extern void* p_PIXEndCapture;
    extern void* p_PIXGetCaptureState;
    extern void* p_SetAppCompatStringPointer;
    extern void* p_UpdateHMDEmulationStatus;
    extern void* p_CreateDXGIFactory;
    extern void* p_CreateDXGIFactory1;
    extern void* p_CreateDXGIFactory2;
    extern void* p_DXGID3D10CreateDevice;
    extern void* p_DXGID3D10CreateLayeredDevice;
    extern void* p_DXGID3D10GetLayeredDeviceSize;
    extern void* p_DXGID3D10RegisterLayers;
    extern void* p_DXGIDeclareAdapterRemovalSupport;
    extern void* p_DXGIDisableVBlankVirtualization;
    extern void* p_DXGIGetDebugInterface1;
    extern void* p_DXGIReportAdapterConfiguration;
}

static HMODULE g_realDxgi = NULL;

typedef HRESULT (WINAPI* CreateDXGIFactory_t)(REFIID, void**);
typedef HRESULT (WINAPI* CreateDXGIFactory2_t)(UINT, REFIID, void**);
static CreateDXGIFactory_t  g_realCreateDXGIFactory  = NULL;
static CreateDXGIFactory_t  g_realCreateDXGIFactory1 = NULL;
static CreateDXGIFactory2_t g_realCreateDXGIFactory2 = NULL;

void Hook_WaitReady();

// The factory exports are the one door every swap chain must pass through;
// they hold until the Present hooks are installed (see Hook_WaitReady).
static HRESULT WINAPI Gate_CreateDXGIFactory(REFIID riid, void** out)
{
    Hook_WaitReady();
    return g_realCreateDXGIFactory(riid, out);
}

static HRESULT WINAPI Gate_CreateDXGIFactory1(REFIID riid, void** out)
{
    Hook_WaitReady();
    return g_realCreateDXGIFactory1(riid, out);
}

static HRESULT WINAPI Gate_CreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    Hook_WaitReady();
    return g_realCreateDXGIFactory2(flags, riid, out);
}

bool Exports_Init()
{
    char sys[MAX_PATH];
    UINT n = GetSystemDirectoryA(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;

    char path[MAX_PATH];
    if (snprintf(path, sizeof(path), "%s\\dxgi.dll", sys) <= 0) return false;

    g_realDxgi = LoadLibraryA(path);
    if (!g_realDxgi) return false;

    struct Entry { const char* name; void** slot; };
    const Entry map[] = {
        { "ApplyCompatResolutionQuirking",    &p_ApplyCompatResolutionQuirking },
        { "CompatString",                     &p_CompatString },
        { "CompatValue",                      &p_CompatValue },
        { "DXGIDumpJournal",                  &p_DXGIDumpJournal },
        { "PIXBeginCapture",                  &p_PIXBeginCapture },
        { "PIXEndCapture",                    &p_PIXEndCapture },
        { "PIXGetCaptureState",               &p_PIXGetCaptureState },
        { "SetAppCompatStringPointer",        &p_SetAppCompatStringPointer },
        { "UpdateHMDEmulationStatus",         &p_UpdateHMDEmulationStatus },
        { "CreateDXGIFactory",                &p_CreateDXGIFactory },
        { "CreateDXGIFactory1",               &p_CreateDXGIFactory1 },
        { "CreateDXGIFactory2",               &p_CreateDXGIFactory2 },
        { "DXGID3D10CreateDevice",            &p_DXGID3D10CreateDevice },
        { "DXGID3D10CreateLayeredDevice",     &p_DXGID3D10CreateLayeredDevice },
        { "DXGID3D10GetLayeredDeviceSize",    &p_DXGID3D10GetLayeredDeviceSize },
        { "DXGID3D10RegisterLayers",          &p_DXGID3D10RegisterLayers },
        { "DXGIDeclareAdapterRemovalSupport", &p_DXGIDeclareAdapterRemovalSupport },
        { "DXGIDisableVBlankVirtualization",  &p_DXGIDisableVBlankVirtualization },
        { "DXGIGetDebugInterface1",           &p_DXGIGetDebugInterface1 },
        { "DXGIReportAdapterConfiguration",   &p_DXGIReportAdapterConfiguration },
    };

    for (const auto& e : map)
        *e.slot = (void*)GetProcAddress(g_realDxgi, e.name);

    // CreateDXGIFactory2 is the one d3d11.dll statically imports - its absence
    // would mean the genuine dxgi is fundamentally wrong. Treat as fatal.
    if (!p_CreateDXGIFactory2) return false;

    // The factory thunks jump into the gates, which forward to the real ones.
    g_realCreateDXGIFactory  = (CreateDXGIFactory_t)p_CreateDXGIFactory;
    g_realCreateDXGIFactory1 = (CreateDXGIFactory_t)p_CreateDXGIFactory1;
    g_realCreateDXGIFactory2 = (CreateDXGIFactory2_t)p_CreateDXGIFactory2;
    if (g_realCreateDXGIFactory)  p_CreateDXGIFactory  = (void*)Gate_CreateDXGIFactory;
    if (g_realCreateDXGIFactory1) p_CreateDXGIFactory1 = (void*)Gate_CreateDXGIFactory1;
    p_CreateDXGIFactory2 = (void*)Gate_CreateDXGIFactory2;
    return true;
}
