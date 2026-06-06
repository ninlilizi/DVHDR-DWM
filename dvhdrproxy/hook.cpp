// hook.cpp — installs the Present interception. dxgi swap chains (whether the
// game drives them through D3D11 or D3D12) share one vtable implemented in
// dxgi.dll, so a single throwaway D3D11 swap chain yields the code addresses of
// Present / Present1 / ResizeBuffers, and MinHook then catches every swap chain
// in the process. A throwaway D3D12 queue gives ExecuteCommandLists, which we
// hook only to learn which DIRECT queue the game presents through (D3D12 has no
// way to recover the queue from the swap chain).

#include "framework.h"
#include "effect_d3d11.h"
#include "effect_d3d12.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void    (STDMETHODCALLTYPE* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

static Present_t             g_presentOrig             = NULL;
static Present1_t            g_present1Orig            = NULL;
static ResizeBuffers_t       g_resizeOrig              = NULL;
static ExecuteCommandLists_t g_execOrig                = NULL;

// SEH-guarded effect application — never let a fault in our pass crash the game.
static void SafeApply(IDXGISwapChain* sc)
{
    __try { if (!Effect11_Apply(sc)) Effect12_Apply(sc); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static HRESULT STDMETHODCALLTYPE Present_hook(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (!(flags & DXGI_PRESENT_TEST)) SafeApply(sc);
    return g_presentOrig(sc, sync, flags);
}

static HRESULT STDMETHODCALLTYPE Present1_hook(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
{
    if (!(flags & DXGI_PRESENT_TEST)) SafeApply(sc);
    return g_present1Orig(sc, sync, flags, params);
}

static HRESULT STDMETHODCALLTYPE ResizeBuffers_hook(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                                   DXGI_FORMAT fmt, UINT flags)
{
    Effect11_OnResize();
    Effect12_OnResize();
    return g_resizeOrig(sc, count, w, h, fmt, flags);
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

// Read Present (8), ResizeBuffers (13), Present1 (22) from a throwaway D3D11
// swap chain's vtable.
static bool GrabSwapChainMethods(void** outPresent, void** outResize, void** outPresent1)
{
    HWND hwnd = MakeDummyWindow();
    if (!hwnd) return false;

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = 1; scd.BufferDesc.Height = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    IDXGISwapChain*      sc  = NULL;
    ID3D11Device*        dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                     &fl, 1, D3D11_SDK_VERSION, &scd, &sc, &dev, NULL, &ctx);
    if (FAILED(hr) || !sc)
    {
        DestroyWindow(hwnd);
        return false;
    }

    void** vt = *(void***)sc;
    *outPresent  = vt[8];
    *outResize   = vt[13];
    *outPresent1 = vt[22];

    if (ctx) ctx->Release();
    if (dev) dev->Release();
    sc->Release();
    DestroyWindow(hwnd);
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

static DWORD WINAPI InstallThread(LPVOID)
{
    void *present = NULL, *resize = NULL, *present1 = NULL, *exec = NULL;
    if (!GrabSwapChainMethods(&present, &resize, &present1))
        return 0;
    GrabExecuteCommandLists(&exec);

    if (MH_Initialize() != MH_OK) return 0;

    MH_CreateHook(present,  (LPVOID)Present_hook,       (LPVOID*)&g_presentOrig);
    MH_CreateHook(resize,   (LPVOID)ResizeBuffers_hook, (LPVOID*)&g_resizeOrig);
    if (present1) MH_CreateHook(present1, (LPVOID)Present1_hook, (LPVOID*)&g_present1Orig);
    if (exec)     MH_CreateHook(exec,    (LPVOID)ExecuteCommandLists_hook, (LPVOID*)&g_execOrig);

    MH_EnableHook(MH_ALL_HOOKS);
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
