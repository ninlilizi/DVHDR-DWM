// display.cpp - what the proxy can learn about a surface beyond its format: the
// colour space the game declared for it, whether it is an opaque chain at all,
// and the state of the output it lands on (HDR mode, and the SDR white level
// Windows composes SDR content at). Everything here runs under the hook lock.

#include "display.h"

// {B7D3E2A1-5C4F-4E8B-9A6D-2F1C3B4A5D6E}
static const GUID kDeclaredColorSpace =
    { 0xb7d3e2a1, 0x5c4f, 0x4e8b, { 0x9a, 0x6d, 0x2f, 0x1c, 0x3b, 0x4a, 0x5d, 0x6e } };

void Display_RecordColorSpace(IDXGISwapChain* sc, DXGI_COLOR_SPACE_TYPE cs)
{
    UINT v = (UINT)cs;
    sc->SetPrivateData(kDeclaredColorSpace, sizeof(v), &v);
}

void Display_ClearRecordedColorSpace(IDXGISwapChain* sc)
{
    sc->SetPrivateData(kDeclaredColorSpace, 0, NULL);
}

bool Display_GetRecordedColorSpace(IDXGISwapChain* sc, DXGI_COLOR_SPACE_TYPE* out)
{
    UINT v = 0, n = sizeof(v);
    if (FAILED(sc->GetPrivateData(kDeclaredColorSpace, &n, &v)) || n != sizeof(v)) return false;
    *out = (DXGI_COLOR_SPACE_TYPE)v;
    return true;
}

// {4C1F9A62-8E3B-4D0A-B7C5-1A2E6F8D9B01}
static const GUID kDeclaredHdrMetadata =
    { 0x4c1f9a62, 0x8e3b, 0x4d0a, { 0xb7, 0xc5, 0x1a, 0x2e, 0x6f, 0x8d, 0x9b, 0x01 } };

struct HdrLevels { float peak; float fall; };

// MaxCLL and MaxFALL are whole nits. Mastering luminance is specified in
// ten-thousandths of a nit, but games have been seen passing whole nits, so
// both readings are tried and the one that lands in a sane range is kept.
void Display_RecordHdrMetadata(IDXGISwapChain* sc, DXGI_HDR_METADATA_TYPE type, UINT size, const void* data)
{
    if (type != DXGI_HDR_METADATA_TYPE_HDR10 || !data || size < sizeof(DXGI_HDR_METADATA_HDR10))
    {
        sc->SetPrivateData(kDeclaredHdrMetadata, 0, NULL);
        return;
    }
    const DXGI_HDR_METADATA_HDR10* m = (const DXGI_HDR_METADATA_HDR10*)data;
    HdrLevels lv = { 0.0f, 0.0f };
    if (m->MaxContentLightLevel > 0) lv.peak = (float)m->MaxContentLightLevel;
    else
    {
        float tenThousandths = (float)m->MaxMasteringLuminance / 10000.0f;
        float whole          = (float)m->MaxMasteringLuminance;
        if (tenThousandths >= 50.0f && tenThousandths <= 10000.0f) lv.peak = tenThousandths;
        else if (whole >= 50.0f && whole <= 10000.0f)            lv.peak = whole;
    }
    lv.fall = (float)m->MaxFrameAverageLightLevel;
    sc->SetPrivateData(kDeclaredHdrMetadata, sizeof(lv), &lv);
}

bool Display_GetRecordedHdrMetadata(IDXGISwapChain* sc, float* peakNits, float* fallNits)
{
    HdrLevels lv = { 0.0f, 0.0f };
    UINT n = sizeof(lv);
    if (FAILED(sc->GetPrivateData(kDeclaredHdrMetadata, &n, &lv)) || n != sizeof(lv) || lv.peak <= 0.0f) return false;
    *peakNits = lv.peak;
    *fallNits = lv.fall;
    return true;
}

// A chain composed with alpha is an overlay (a UI layer over a video, say), not
// picture content: tonemapping premultiplied colour is wrong and writing its
// alpha would blank whatever sits beneath it.
bool Display_IsOpaqueChain(IDXGISwapChain* sc)
{
    IDXGISwapChain1* sc1 = NULL;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) || !sc1) return true; // legacy chain: always opaque
    DXGI_SWAP_CHAIN_DESC1 d;
    bool opaque = true;
    if (SUCCEEDED(sc1->GetDesc1(&d)))
        opaque = (d.AlphaMode != DXGI_ALPHA_MODE_PREMULTIPLIED && d.AlphaMode != DXGI_ALPHA_MODE_STRAIGHT);
    sc1->Release();
    return opaque;
}

// Windows reports the SDR white level per display path in thousandths of 80 nits.
static float SdrWhiteLevelForMonitor(HMONITOR mon)
{
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return 0.0f;

    UINT32 nPaths = 0, nModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPaths, &nModes) != ERROR_SUCCESS) return 0.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPaths, paths.data(), &nModes, modes.data(), NULL) != ERROR_SUCCESS)
        return 0.0f;

    for (UINT32 i = 0; i < nPaths; i++)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
        src.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size      = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id        = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL wl = {};
        wl.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        wl.header.size      = sizeof(wl);
        wl.header.adapterId = paths[i].targetInfo.adapterId;
        wl.header.id        = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&wl.header) != ERROR_SUCCESS) return 0.0f;
        return (float)wl.SDRWhiteLevel * (80.0f / 1000.0f);
    }
    return 0.0f;
}

// Locate the output that owns a monitor through the swap chain's own factory.
static bool OutputDescForMonitor(IDXGISwapChain* sc, HMONITOR mon, DXGI_OUTPUT_DESC1* out)
{
    IDXGIFactory* factory = NULL;
    if (FAILED(sc->GetParent(IID_PPV_ARGS(&factory))) || !factory) return false;

    bool found = false;
    IDXGIAdapter* adapter = NULL;
    for (UINT a = 0; !found && factory->EnumAdapters(a, &adapter) == S_OK; a++)
    {
        IDXGIOutput* output = NULL;
        for (UINT o = 0; !found && adapter->EnumOutputs(o, &output) == S_OK; o++)
        {
            IDXGIOutput6* out6 = NULL;
            if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out6))) && out6)
            {
                DXGI_OUTPUT_DESC1 d;
                if (SUCCEEDED(out6->GetDesc1(&d)) && d.Monitor == mon) { *out = d; found = true; }
                out6->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

static bool ResolveOutput(IDXGISwapChain* sc, DXGI_OUTPUT_DESC1* out)
{
    IDXGIOutput* output = NULL;
    if (SUCCEEDED(sc->GetContainingOutput(&output)) && output)
    {
        bool ok = false;
        IDXGIOutput6* out6 = NULL;
        if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out6))) && out6)
        {
            ok = SUCCEEDED(out6->GetDesc1(out));
            out6->Release();
        }
        output->Release();
        if (ok) return true;
    }

    // Composition chains have no containing output; go by the window instead.
    DXGI_SWAP_CHAIN_DESC scd;
    if (FAILED(sc->GetDesc(&scd)) || !scd.OutputWindow) return false;
    HMONITOR mon = MonitorFromWindow(scd.OutputWindow, MONITOR_DEFAULTTONEAREST);
    return mon && OutputDescForMonitor(sc, mon, out);
}

// One cache entry per swap chain, so chains presenting in alternation do not
// evict each other every frame.
struct DisplayCache
{
    IDXGISwapChain* key = NULL;
    ULONGLONG       at  = 0;
    DisplayState    st  = {};
};
static const int  kCacheSlots = 4;
static DisplayCache g_cache[kCacheSlots];

DisplayState Display_Query(IDXGISwapChain* sc)
{
    ULONGLONG now = GetTickCount64();
    DisplayCache* e = NULL;
    DisplayCache* victim = &g_cache[0];
    for (int i = 0; i < kCacheSlots; i++)
    {
        if (g_cache[i].key == sc) { e = &g_cache[i]; break; }
        if (g_cache[i].at < victim->at) victim = &g_cache[i];
    }
    if (e && now - e->at < 1000) return e->st;
    if (!e) e = victim;

    DisplayState st = {};
    DXGI_OUTPUT_DESC1 d;
    if (ResolveOutput(sc, &d))
    {
        st.Known   = true;
        st.HdrMode = (d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        if (st.HdrMode) st.SdrWhiteNits = SdrWhiteLevelForMonitor(d.Monitor);
    }

    e->key = sc; e->at = now; e->st = st;
    return st;
}
