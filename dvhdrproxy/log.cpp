#include "log.h"
#include <stdarg.h>

static bool     g_enabled = false;
static FILE*    g_file    = NULL;
static SRWLOCK  g_lock    = SRWLOCK_INIT;
static ULONGLONG g_t0     = 0;

bool Log_Enabled() { return g_enabled; }

// A process that may write no file at all (a sandboxed GPU process) still
// speaks through OutputDebugString, where a debugger or DebugView can hear it.
void Log_Write(const char* fmt, ...)
{
    if (!g_enabled) return;
    char line[1024];
    int n = snprintf(line, sizeof(line), "%8llu ms  t%-6lu ", GetTickCount64() - g_t0, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);

    AcquireSRWLockExclusive(&g_lock);
    if (g_file)
    {
        fputs(line, g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
    else
    {
        strncat(line, "\n", sizeof(line) - strlen(line) - 1);
        OutputDebugStringA(line);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// One file per process (a browser runs dozens of helpers from the same exe),
// in the first folder this process is allowed to write to: a sandboxed GPU
// process runs at low integrity and cannot touch the ordinary temp folder.
static FILE* OpenLogFile(const char* exeName)
{
    char dirs[3][MAX_PATH] = {};
    GetTempPathA(MAX_PATH, dirs[0]);
    char profile[MAX_PATH] = {};
    if (GetEnvironmentVariableA("USERPROFILE", profile, MAX_PATH))
        snprintf(dirs[1], MAX_PATH, "%s\\AppData\\LocalLow\\", profile);
    GetWindowsDirectoryA(dirs[2], MAX_PATH);
    strncat(dirs[2], "\\Temp\\", MAX_PATH - strlen(dirs[2]) - 1);

    for (int i = 0; i < 3; i++)
    {
        if (!dirs[i][0]) continue;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%sdvhdrproxy-%s-%lu.log", dirs[i], exeName, GetCurrentProcessId());
        FILE* f = fopen(path, "w");
        if (f) return f;
    }
    return NULL;
}

void Log_Init(bool enabled)
{
    g_enabled = enabled;
    if (!enabled) return;

    char exe[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    const char* name = strrchr(exe, '\\');
    name = name ? name + 1 : exe;

    g_file = OpenLogFile(name);
    g_t0 = GetTickCount64();

    char cmd[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, GetCommandLineW(), -1, cmd, sizeof(cmd) - 1, NULL, NULL);
    Log_Write("dvhdrproxy log for %s (pid %lu)%s", exe, GetCurrentProcessId(),
              g_file ? "" : " - no writable log folder, lines go to OutputDebugString");
    Log_Write("command line: %s", cmd);
}

void Log_Shutdown()
{
    if (g_file) { fclose(g_file); g_file = NULL; }
    g_enabled = false;
}

static const char* SwapEffectName(DXGI_SWAP_EFFECT e)
{
    switch (e)
    {
    case DXGI_SWAP_EFFECT_DISCARD:         return "DISCARD";
    case DXGI_SWAP_EFFECT_SEQUENTIAL:      return "SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL: return "FLIP_SEQUENTIAL";
    case DXGI_SWAP_EFFECT_FLIP_DISCARD:    return "FLIP_DISCARD";
    default:                               return "?";
    }
}

static const char* AlphaModeName(DXGI_ALPHA_MODE a)
{
    switch (a)
    {
    case DXGI_ALPHA_MODE_UNSPECIFIED:   return "UNSPECIFIED";
    case DXGI_ALPHA_MODE_PREMULTIPLIED: return "PREMULTIPLIED";
    case DXGI_ALPHA_MODE_STRAIGHT:      return "STRAIGHT";
    case DXGI_ALPHA_MODE_IGNORE:        return "IGNORE";
    default:                            return "?";
    }
}

static void DescribeChain(IDXGISwapChain* sc)
{
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(sc->GetDesc(&d)))
    {
        char title[64] = "";
        if (d.OutputWindow) GetWindowTextA(d.OutputWindow, title, sizeof(title));
        Log_Write("chain %p: hwnd=%p \"%s\" buffers=%u effect=%s usage=0x%x flags=0x%x windowed=%d",
                  sc, d.OutputWindow, title, d.BufferCount, SwapEffectName(d.SwapEffect),
                  d.BufferUsage, d.Flags, (int)d.Windowed);
    }
    IDXGISwapChain1* sc1 = NULL;
    if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1))) && sc1)
    {
        DXGI_SWAP_CHAIN_DESC1 d1;
        if (SUCCEEDED(sc1->GetDesc1(&d1)))
            Log_Write("chain %p: alpha=%s scaling=%d stereo=%d samples=%u%s",
                      sc, AlphaModeName(d1.AlphaMode), (int)d1.Scaling, (int)d1.Stereo, d1.SampleDesc.Count,
                      d.OutputWindow ? "" : " (composition chain, no window)");
        sc1->Release();
    }
}

struct ChainRecord
{
    IDXGISwapChain* key = NULL;
    char            sig[192] = {};
};
static const int   kRecords = 8;
static ChainRecord g_records[kRecords];
static int         g_nextRecord = 0;

void Log_Chain(IDXGISwapChain* sc, const char* api, const void* device,
               DXGI_FORMAT fmt, UINT w, UINT h, const char* verdict)
{
    if (!g_enabled) return;

    char sig[192];
    snprintf(sig, sizeof(sig), "%s dev=%p fmt=%d %ux%u -> %s", api, device, (int)fmt, w, h, verdict ? verdict : "?");

    ChainRecord* r = NULL;
    for (int i = 0; i < kRecords; i++)
        if (g_records[i].key == sc) { r = &g_records[i]; break; }

    bool fresh = (r == NULL);
    if (fresh)
    {
        r = &g_records[g_nextRecord];
        g_nextRecord = (g_nextRecord + 1) % kRecords;
        r->key = sc;
        r->sig[0] = '\0';
    }
    if (strcmp(r->sig, sig) == 0) return;
    strncpy(r->sig, sig, sizeof(r->sig) - 1);

    if (fresh) DescribeChain(sc);
    Log_Write("chain %p: %s", sc, sig);
}
