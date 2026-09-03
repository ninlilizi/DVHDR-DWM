// hook.cpp — installs the Present interception. dxgi swap chains (whether the
// game drives them through D3D11 or D3D12) share one vtable implemented in
// dxgi.dll, so a single throwaway D3D11 swap chain yields the code addresses of
// Present / Present1 / ResizeBuffers / SetColorSpace1 / SetHDRMetaData, and MinHook then catches
// every swap chain in the process. A throwaway D3D12 queue gives
// ExecuteCommandLists, which we hook only to learn which DIRECT queue the game
// presents through (D3D12 has no way to recover the queue from the swap chain).

#include "framework.h"
#include "effect_d3d11.h"
#include "effect_d3d12.h"
#include "display.h"
#include "log.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* SetColorSpace1_t)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
typedef HRESULT (STDMETHODCALLTYPE* SetHDRMetaData_t)(IDXGISwapChain4*, DXGI_HDR_METADATA_TYPE, UINT, void*);
typedef void    (STDMETHODCALLTYPE* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

static Present_t             g_presentOrig             = NULL;
static Present1_t            g_present1Orig            = NULL;
static ResizeBuffers_t       g_resizeOrig              = NULL;
static SetColorSpace1_t      g_setColorSpaceOrig       = NULL;
static SetHDRMetaData_t      g_setHdrMetaOrig          = NULL;
static ExecuteCommandLists_t g_execOrig                = NULL;

// Present and ResizeBuffers arrive from whichever threads the application
// drives its swap chains on; the effect state is only ever touched under this.
static SRWLOCK g_fxLock = SRWLOCK_INIT;

// SEH-guarded effect application - never let a fault in our pass crash the game.
// Returns true when the back buffer was rewritten.
static bool SafeApply(IDXGISwapChain* sc, const DXGI_PRESENT_PARAMETERS* present)
{
    bool applied = false;
    AcquireSRWLockExclusive(&g_fxLock);
    __try { applied = Effect11_Apply(sc, present) || Effect12_Apply(sc, present); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    ReleaseSRWLockExclusive(&g_fxLock);
    return applied;
}

static HRESULT STDMETHODCALLTYPE Present_hook(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (!(flags & DXGI_PRESENT_TEST)) SafeApply(sc, NULL);
    return g_presentOrig(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE Present1_hook(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
{
    if (!(flags & DXGI_PRESENT_TEST) && SafeApply(sc, params) && params)
    {
        // The pass rewrote the whole back buffer, so the whole frame is what
        // must reach the screen: a dirty-rectangle present would let DXGI stitch
        // the previous frame back in around them.
        DXGI_PRESENT_PARAMETERS whole = {};
        return g_present1Orig(sc, sync, flags, &whole);
    }
    return g_present1Orig(sc, sync, flags, params);
}

// A colour space declared for the old buffers means nothing once they change
// format; forget it so the next Present classifies the new ones afresh.
static void ForgetStaleDeclaration(IDXGISwapChain* sc, DXGI_FORMAT requested)
{
    if (requested == DXGI_FORMAT_UNKNOWN) return;   // keep the current format
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(sc->GetDesc(&d)) && d.BufferDesc.Format != requested)
    {
        Display_ClearRecordedColorSpace(sc);
        Log_Write("chain %p: ResizeBuffers changes format %d -> %d, declaration forgotten", sc, (int)d.BufferDesc.Format, (int)requested);
    }
}

static HRESULT STDMETHODCALLTYPE ResizeBuffers_hook(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                                   DXGI_FORMAT fmt, UINT flags)
{
    Log_Write("chain %p: ResizeBuffers count=%u %ux%u fmt=%d flags=0x%x", sc, count, w, h, (int)fmt, flags);
    AcquireSRWLockExclusive(&g_fxLock);
    __try
    {
        ForgetStaleDeclaration(sc, fmt);
        Effect11_OnResize(sc);
        Effect12_OnResize(sc);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    ReleaseSRWLockExclusive(&g_fxLock);
    return g_resizeOrig(sc, count, w, h, fmt, flags);
}

// The declaration is the one authoritative statement of what the back buffer
// holds; remember it on the swap chain so classification need not guess from
// the format alone.
static HRESULT STDMETHODCALLTYPE SetColorSpace1_hook(IDXGISwapChain3* sc, DXGI_COLOR_SPACE_TYPE cs)
{
    HRESULT hr = g_setColorSpaceOrig(sc, cs);
    if (SUCCEEDED(hr)) Display_RecordColorSpace(sc, cs);
    Log_Write("chain %p: SetColorSpace1(%d) hr=0x%08x", sc, (int)cs, (unsigned)hr);
    return hr;
}

// Mastering metadata names the content's own peak; it seeds the adaptation
// before the histogram has had a frame to measure it.
static HRESULT STDMETHODCALLTYPE SetHDRMetaData_hook(IDXGISwapChain4* sc, DXGI_HDR_METADATA_TYPE type,
                                                    UINT size, void* data)
{
    HRESULT hr = g_setHdrMetaOrig(sc, type, size, data);
    if (SUCCEEDED(hr)) Display_RecordHdrMetadata(sc, type, size, data);
    float peak = 0.0f, fall = 0.0f;
    bool have = Display_GetRecordedHdrMetadata(sc, &peak, &fall);
    Log_Write("chain %p: SetHDRMetaData(type %d, %u bytes) hr=0x%08x -> peak %.0f fall %.0f nits",
              sc, (int)type, size, (unsigned)hr, have ? peak : 0.0f, have ? fall : 0.0f);
    return hr;
}

static void STDMETHODCALLTYPE ExecuteCommandLists_hook(ID3D12CommandQueue* q, UINT n,
                                                      ID3D12CommandList* const* lists)
{
    Effect12_SetQueue(q);
    g_execOrig(q, n, lists);
}

static ATOM        g_wndClass = 0;
static const wchar_t* kWndClassName = L"DvhdrProxyDummyWnd";

static HWND MakeDummyWindow()
{
    if (!g_wndClass)
    {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.lpszClassName = kWndClassName;
        g_wndClass = RegisterClassExW(&wc);
    }
    return CreateWindowExW(0, kWndClassName, L"", WS_OVERLAPPEDWINDOW,
                           0, 0, 1, 1, NULL, NULL, GetModuleHandleW(NULL), NULL);
}

// A throwaway swap chain whose vtable we can read. A windowless composition
// chain is tried first: it needs no user32 window, which a sandboxed process (a
// browser's GPU process) is forbidden to create. A window-bound chain is the
// fallback for systems without DXGI 1.2.
static IDXGISwapChain* MakeThrowawayChain(ID3D11Device* dev, HWND* outWnd)
{
    IDXGISwapChain* sc = NULL;
    *outWnd = NULL;

    IDXGIDevice*   dxgiDev = NULL;
    IDXGIAdapter*  adapter = NULL;
    IDXGIFactory2* factory = NULL;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev))) && dxgiDev
        && SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter
        && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory))) && factory)
    {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = 8; d.Height = 8;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        IDXGISwapChain1* sc1 = NULL;
        if (SUCCEEDED(factory->CreateSwapChainForComposition(dev, &d, NULL, &sc1)) && sc1)
            sc = sc1;

        if (!sc)
        {
            *outWnd = MakeDummyWindow();
            if (*outWnd)
            {
                d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                if (SUCCEEDED(factory->CreateSwapChainForHwnd(dev, *outWnd, &d, NULL, NULL, &sc1)) && sc1)
                    sc = sc1;
            }
        }
    }
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgiDev) dxgiDev->Release();
    return sc;
}

// Read Present (8), ResizeBuffers (13), Present1 (22), SetColorSpace1 (38, via
// IDXGISwapChain3) and SetHDRMetaData (40, via IDXGISwapChain4) from a throwaway
// swap chain's vtable.
static bool GrabSwapChainMethods(void** outPresent, void** outResize, void** outPresent1,
                                 void** outSetColorSpace, void** outSetHdrMeta)
{
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device*        dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1, D3D11_SDK_VERSION,
                                 &dev, NULL, &ctx)) || !dev)
        return false;

    HWND hwnd = NULL;
    IDXGISwapChain* sc = MakeThrowawayChain(dev, &hwnd);
    if (!sc)
    {
        if (ctx) ctx->Release();
        dev->Release();
        if (hwnd) DestroyWindow(hwnd);
        return false;
    }

    void** vt = *(void***)sc;
    *outPresent  = vt[8];
    *outResize   = vt[13];
    *outPresent1 = vt[22];

    *outSetColorSpace = NULL;
    IDXGISwapChain3* sc3 = NULL;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3)
    {
        *outSetColorSpace = (*(void***)sc3)[38];
        sc3->Release();
    }

    *outSetHdrMeta = NULL;
    IDXGISwapChain4* sc4 = NULL;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc4))) && sc4)
    {
        *outSetHdrMeta = (*(void***)sc4)[40];
        sc4->Release();
    }

    sc->Release();
    if (ctx) ctx->Release();
    dev->Release();
    if (hwnd) DestroyWindow(hwnd);
    return true;
}

// Read ExecuteCommandLists (10) from a throwaway D3D12 DIRECT queue's vtable.
// Absent D3D12 support, leaves *out null and the caller skips that hook.
static void GrabExecuteCommandLists(void** out)
{
    *out = NULL;
    ID3D12Device* dev = NULL;
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) || !dev)
        return;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = NULL;
    if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) && q)
    {
        void** vt = *(void***)q;
        *out = vt[10];
        q->Release();
    }
    dev->Release();
}

// A Chromium-style helper process (renderer, utility, network...) never
// presents; only the GPU process and the main process are worth hooking.
static bool IsHelperProcess()
{
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd || !wcsstr(cmd, L"--type=")) return false;
    return wcsstr(cmd, L"--type=gpu-process") == NULL;
}

static DWORD WINAPI InstallThread(LPVOID)
{
    if (IsHelperProcess())
    {
        Log_Write("helper process, hooks not installed");
        return 0;
    }
    void *present = NULL, *resize = NULL, *present1 = NULL, *setCsp = NULL, *setMeta = NULL, *exec = NULL;
    if (!GrabSwapChainMethods(&present, &resize, &present1, &setCsp, &setMeta))
    {
        Log_Write("could not create a throwaway swap chain, hooks not installed");
        return 0;
    }
    GrabExecuteCommandLists(&exec);

    if (MH_Initialize() != MH_OK) return 0;

    MH_CreateHook(present,  (LPVOID)Present_hook,       (LPVOID*)&g_presentOrig);
    MH_CreateHook(resize,   (LPVOID)ResizeBuffers_hook, (LPVOID*)&g_resizeOrig);
    if (present1) MH_CreateHook(present1, (LPVOID)Present1_hook, (LPVOID*)&g_present1Orig);
    if (setCsp)   MH_CreateHook(setCsp,   (LPVOID)SetColorSpace1_hook, (LPVOID*)&g_setColorSpaceOrig);
    if (setMeta)  MH_CreateHook(setMeta,  (LPVOID)SetHDRMetaData_hook, (LPVOID*)&g_setHdrMetaOrig);
    if (exec)     MH_CreateHook(exec,    (LPVOID)ExecuteCommandLists_hook, (LPVOID*)&g_execOrig);

    MH_EnableHook(MH_ALL_HOOKS);
    Log_Write("hooks installed: Present=%p Present1=%p ResizeBuffers=%p SetColorSpace1=%p SetHDRMetaData=%p ExecuteCommandLists=%p",
              present, present1, resize, setCsp, setMeta, exec);
    return 0;
}

void Hook_Start()
{
    HANDLE h = CreateThread(NULL, 0, InstallThread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

void Hook_Stop()
{
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}
