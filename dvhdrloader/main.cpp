// dvhdrloader/main.cpp — Task-Scheduler-friendly companion that ensures
// dvhdr.dll is loaded into the current-session dwm.exe. Idempotent: a
// no-args run is a no-op when the DLL is already present.
//
// Built as a Windows-subsystem executable: Task Scheduler runs it in the
// logged-on session without flashing a console window. Interactive runs attach
// to the launching terminal's console (see AttachParentConsole) so the query
// commands still print.
//
// Injection model — mirrors lauralex/dwm_lut's DwmLutGUI.Injector:
//   1. Copy dvhdr.dll from beside the loader to %SYSTEMROOT%\Temp\dvhdr.dll
//      (a trusted-zone path the DWM-N virtual account can read), but only if
//      the destination is missing or older than the source — skip otherwise
//      to avoid write-thrashing every invocation.
//   2. Strip the DACL on the installed copy so DWM-N can definitely open it.
//   3. Impersonate SYSTEM via lsass.exe's token. dwm.exe is owned by the
//      virtual DWM account and a plain admin token can't perform a full-rights
//      OpenProcess + CreateRemoteThread; running the inject thread as SYSTEM
//      makes the call succeed for the same reason it does for the reference.
//   4. Standard VirtualAllocEx + WriteProcessMemory + CreateRemoteThread→
//      LoadLibraryA, then RevertToSelf.
//
// Monitor selection lives in HKLM\SOFTWARE\DVHDR-DWM\Monitors as a
// REG_MULTI_SZ list of "left,top,index" strings. The DLL reads it on attach and
// matches each DWM context by (left,top); the index lets it pull that screen's
// per-monitor [Display.N] capability overrides from the ini. No side files
// beside the loader.
//
// Flags:
//   (none)            inject if absent, exit silently if already present
//   --force           unload + reinject (forces config reload)
//   --unload          remove the DLL from dwm.exe
//   --status          report whether dvhdr.dll is currently loaded
//   --list            enumerate displays with index + coords, exit
//   -m N[,N,...]      write monitor coords to registry for the given display
//                     number(s), then force-reinject. Several screens may be
//                     tonemapped at once; each can carry its own capabilities in
//                     a [Display.N] ini section. Mirrors ApplyIccLut -m.
//   --dim N[,N,...]   idle-screen dimmer (a separate, persistent user-session
//                     feature — NOT the injected shader): watch the given display
//                     number(s) and gradually fade a black overlay over each one
//                     after [Dimmer] IdleSeconds of no on-screen change, lifting
//                     it again on activity. Toggle at runtime with the [Dimmer]
//                     ToggleHotkey or the tray icon. See the [Dimmer] ini section.
//   --dim-stop        signal a running --dim watcher to exit
//   -q/--silent       suppress console output

#include "pch.h"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#define DVHDR_REG_PATH    L"SOFTWARE\\DVHDR-DWM"
#define DVHDR_REG_VALUE   L"Monitors"

static bool g_silent = false;

static bool g_attached  = false;  // attached to a parent shell's console
static bool g_gapOpened = false;

// When attached to a shell's console, our first line would otherwise print on
// the same row as the prompt. Emit a one-time blank line before the first
// output, and (via atexit) a matching one after the last, so the run sits
// cleanly between the surrounding prompts.
static void CloseOutputGap() { if (g_gapOpened) printf("\n"); }
static void OpenOutputGap()
{
    if (g_silent || !g_attached || g_gapOpened) return;
    printf("\n");
    g_gapOpened = true;
    atexit(CloseOutputGap);
}

// The loader is built as a Windows-subsystem executable so Task Scheduler runs
// it in the logged-on session without ever spawning a console window. When it's
// instead launched from an interactive terminal, attach to that parent console
// and rebind the std streams so --list/--status/--help still print. No parent
// console (scheduler task, double-click) means output simply goes nowhere.
static void AttachParentConsole()
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    g_attached = true;
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$",  "r", stdin);
}

static void msg(const char* fmt, ...)
{
    if (g_silent) return;
    OpenOutputGap();
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static bool GetSiblingPath(const char* leaf, char* out, size_t cap)
{
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n == cap) return false;
    char* slash = strrchr(out, '\\');
    if (!slash) return false;
    *(slash + 1) = '\0';
    if (strlen(out) + strlen(leaf) + 1 > cap) return false;
    strcat(out, leaf);
    return true;
}

struct DisplayInfo
{
    int index;          // the "Display N" number Windows Settings shows
    int left, top;      // virtual-screen coords of the monitor's top-left
    int width, height;
    std::wstring deviceName;
    std::wstring friendly;
    bool primary;
};

static std::vector<DisplayInfo> EnumDisplays()
{
    std::vector<DisplayInfo> out;
    DISPLAY_DEVICEW dd = {}; dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++)
    {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;

        DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) continue;

        DisplayInfo di = {};
        const wchar_t* p = wcsstr(dd.DeviceName, L"DISPLAY");
        di.index      = p ? _wtoi(p + 7) : (int)(i + 1);
        di.left       = dm.dmPosition.x;
        di.top        = dm.dmPosition.y;
        di.width      = dm.dmPelsWidth;
        di.height     = dm.dmPelsHeight;
        di.deviceName = dd.DeviceName;
        di.primary    = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;

        DISPLAY_DEVICEW dd2 = {}; dd2.cb = sizeof(dd2);
        if (EnumDisplayDevicesW(dd.DeviceName, 0, &dd2, 0))
            di.friendly = dd2.DeviceString;

        out.push_back(di);
    }
    return out;
}

static void PrintDisplays(const std::vector<DisplayInfo>& ds)
{
    if (g_silent) return;
    OpenOutputGap();
    printf("Display  Coords (left,top)   Size           Friendly name\n");
    printf("-------  ------------------  -------------  -------------------\n");
    for (auto& d : ds)
        printf("%-7d  (%6d, %6d)     %4d x %-6d   %ls%s\n",
               d.index, d.left, d.top, d.width, d.height,
               d.friendly.c_str(), d.primary ? " (primary)" : "");
}

// HKLM\SOFTWARE\DVHDR-DWM\Monitors = REG_MULTI_SZ list of "left,top,index"
// strings. The payload matches a DWM context by its (left,top) origin and uses
// the index to read that screen's [Display.N] capability overrides from the ini.
static bool WriteMonitorsToRegistry(const std::vector<DisplayInfo>& selected)
{
    std::vector<wchar_t> buf;
    for (auto& d : selected)
    {
        wchar_t line[64];
        swprintf_s(line, L"%d,%d,%d", d.left, d.top, d.index);
        for (wchar_t* p = line; *p; p++) buf.push_back(*p);
        buf.push_back(L'\0');
    }
    buf.push_back(L'\0'); // double-null terminator for REG_MULTI_SZ
    if (buf.size() == 1) buf.push_back(L'\0'); // empty multi-sz still needs two nulls

    HKEY key;
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, DVHDR_REG_PATH, 0, NULL,
                              REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) return false;
    rc = RegSetValueExW(key, DVHDR_REG_VALUE, 0, REG_MULTI_SZ,
                        (const BYTE*)buf.data(),
                        (DWORD)(buf.size() * sizeof(wchar_t)));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static bool GetSourceDllPath(char* out, size_t cap)
{
    if (!GetSiblingPath("dvhdr.dll", out, cap)) return false;
    return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
}

// %SYSTEMROOT%\Temp\dvhdr.dll — the path injected into dwm.exe. Trusted-zone
// for the DWM-N virtual account; same pattern as the reference fork.
static bool GetInstalledDllPath(char* out, size_t cap)
{
    char base[MAX_PATH];
    DWORD n = GetWindowsDirectoryA(base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    int len = snprintf(out, cap, "%s\\Temp\\dvhdr.dll", base);
    return len > 0 && (size_t)len < cap;
}

// True if dst exists and matches src in size + last-write-time. CopyFileA
// preserves the source's last-write-time, so equality means the dest came
// from this exact source — safe to skip the copy.
static bool IsInstalledUpToDate(const char* src, const char* dst)
{
    WIN32_FILE_ATTRIBUTE_DATA s = {}, d = {};
    if (!GetFileAttributesExA(src, GetFileExInfoStandard, &s)) return false;
    if (!GetFileAttributesExA(dst, GetFileExInfoStandard, &d)) return false;
    if (s.nFileSizeHigh != d.nFileSizeHigh) return false;
    if (s.nFileSizeLow  != d.nFileSizeLow)  return false;
    return CompareFileTime(&s.ftLastWriteTime, &d.ftLastWriteTime) == 0;
}

// Wipe the DACL on the file so the DWM-N account can definitely read it,
// regardless of any restrictive ACL inherited from the parent directory.
static void ClearDacl(const char* path)
{
    HANDLE h = CreateFileA(path, READ_CONTROL | WRITE_DAC, 0, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                    NULL, NULL, NULL, NULL);
    CloseHandle(h);
}

// Copy source → installed if the installed copy is missing or stale. The
// up-to-date check avoids touching disk every Task Scheduler tick.
// Returns true if the installed copy is present and current after the call.
static bool EnsureFileInstalled(const char* src, const char* dst, bool* outCopied)
{
    if (outCopied) *outCopied = false;
    if (IsInstalledUpToDate(src, dst)) return true;

    // The dwm.exe-loaded copy may hold an FS lock — caller should have unloaded
    // first if a refresh was intended.
    if (!CopyFileA(src, dst, FALSE)) return false;
    ClearDacl(dst);
    if (outCopied) *outCopied = true;
    return true;
}

// Resolve installed-sibling path under %SYSTEMROOT%\Temp\.
static bool GetInstalledSiblingPath(const char* leaf, char* out, size_t cap)
{
    char base[MAX_PATH];
    DWORD n = GetWindowsDirectoryA(base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    int len = snprintf(out, cap, "%s\\Temp\\%s", base, leaf);
    return len > 0 && (size_t)len < cap;
}

// Install all three sibling files (DLL + INI + HLSL) into %SYSTEMROOT%\Temp\
// from beside the loader. Each is independently size+mtime-checked, so a tick
// that finds everything current touches no disk.
static bool EnsurePayloadInstalled(bool* outAnyCopied)
{
    if (outAnyCopied) *outAnyCopied = false;
    static const char* leaves[] = { "dvhdr.dll", "dvhdr.ini" };
    for (auto* leaf : leaves)
    {
        char src[MAX_PATH], dst[MAX_PATH];
        if (!GetSiblingPath(leaf, src, sizeof(src))
            || GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES)
        {
            msg("%s not found next to loader", leaf);
            return false;
        }
        if (!GetInstalledSiblingPath(leaf, dst, sizeof(dst))) return false;
        bool copied = false;
        if (!EnsureFileInstalled(src, dst, &copied))
        {
            msg("Could not install %s (%lu)", leaf, GetLastError());
            return false;
        }
        if (copied)
        {
            if (outAnyCopied) *outAnyCopied = true;
            msg("Installed %s", dst);
        }
    }
    return true;
}

// Best-effort removal of all installed sibling files (paired with --unload).
static void RemovePayloadInstalled()
{
    static const char* leaves[] = { "dvhdr.dll", "dvhdr.ini" };
    for (auto* leaf : leaves)
    {
        char dst[MAX_PATH];
        if (GetInstalledSiblingPath(leaf, dst, sizeof(dst))) DeleteFileA(dst);
    }
}

static DWORD FindDwmInCurrentSession()
{
    DWORD selfSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &selfSession);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"dwm.exe") != 0) continue;
            DWORD s = 0;
            if (ProcessIdToSessionId(pe.th32ProcessID, &s) && s == selfSession)
            {
                found = pe.th32ProcessID;
                break;
            }
        }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Match by basename so a previous load from a different directory still
// resolves — relevant after we migrate the install location.
static HMODULE FindLoadedModule(HANDLE proc, const char* dllLeaf)
{
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModulesEx(proc, mods, sizeof(mods), &need, LIST_MODULES_64BIT)) return NULL;
    DWORD count = need / sizeof(HMODULE);
    for (DWORD i = 0; i < count; i++)
    {
        char buf[MAX_PATH];
        if (GetModuleFileNameExA(proc, mods[i], buf, sizeof(buf)) == 0) continue;
        const char* slash = strrchr(buf, '\\');
        const char* leaf = slash ? slash + 1 : buf;
        if (_stricmp(leaf, dllLeaf) == 0) return mods[i];
    }
    return NULL;
}

static bool EnableDebugPrivilege()
{
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    TOKEN_PRIVILEGES tp = {};
    if (!LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) { CloseHandle(tok); return false; }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(tok);
    return ok && GetLastError() == ERROR_SUCCESS;
}

static DWORD FindProcessByName(const wchar_t* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Impersonate SYSTEM by stealing lsass.exe's token. Required because dwm.exe
// is owned by the DWM-N virtual account; an admin token can OpenProcess but
// cannot drive a full-rights CreateRemoteThread. Caller must RevertToSelf
// when done. Requires SeDebugPrivilege.
static bool ImpersonateSystem()
{
    DWORD pid = FindProcessByName(L"lsass.exe");
    if (!pid) { msg("lsass.exe not found"); return false; }

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) { msg("OpenProcess(lsass.exe) failed (%lu)", GetLastError()); return false; }

    HANDLE tok = NULL;
    BOOL gotToken = OpenProcessToken(proc, MAXIMUM_ALLOWED, &tok);
    CloseHandle(proc);
    if (!gotToken) { msg("OpenProcessToken(lsass.exe) failed (%lu)", GetLastError()); return false; }

    BOOL imp = ImpersonateLoggedOnUser(tok);
    CloseHandle(tok);
    if (!imp) { msg("ImpersonateLoggedOnUser failed (%lu)", GetLastError()); return false; }

    // Sanity check — same belt-and-braces the reference does.
    wchar_t name[256]; DWORD sz = 256;
    if (!GetUserNameW(name, &sz) || _wcsicmp(name, L"SYSTEM") != 0)
    {
        msg("Impersonation succeeded but token is not SYSTEM (%ls)", name);
        RevertToSelf();
        return false;
    }
    return true;
}

static bool RemoteCall(HANDLE proc, LPTHREAD_START_ROUTINE fn, void* arg, DWORD* outExit)
{
    HANDLE th = CreateRemoteThread(proc, NULL, 0, fn, arg, 0, NULL);
    if (!th) return false;
    WaitForSingleObject(th, 15000);
    DWORD ec = 0;
    GetExitCodeThread(th, &ec);
    CloseHandle(th);
    if (outExit) *outExit = ec;
    return true;
}

static bool Inject(HANDLE proc, const char* dllPath)
{
    size_t bytes = strlen(dllPath) + 1;
    void* remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { msg("VirtualAllocEx failed (%lu)", GetLastError()); return false; }

    if (!WriteProcessMemory(proc, remote, dllPath, bytes, NULL))
    {
        msg("WriteProcessMemory failed (%lu)", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
    DWORD ec = 0;
    bool ok = RemoteCall(proc, loadLib, remote, &ec);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);

    if (!ok) { msg("CreateRemoteThread(LoadLibraryA) failed (%lu)", GetLastError()); return false; }
    if (ec == 0) { msg("LoadLibraryA returned NULL in dwm.exe"); return false; }
    return true;
}

static bool Unload(HANDLE proc, HMODULE remoteModule)
{
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto freeLib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "FreeLibrary");
    DWORD ec = 0;
    if (!RemoteCall(proc, freeLib, remoteModule, &ec))
    {
        msg("CreateRemoteThread(FreeLibrary) failed (%lu)", GetLastError());
        return false;
    }
    return ec != 0;
}

// ===========================================================================
// Idle-screen dimmer — `dvhdrloader --dim N[,N,...]`.
//
// A self-contained, user-session feature, independent of the DLL injection
// above. Each chosen display is watched with the DXGI Desktop Duplication API
// purely as a change signal: AcquireNextFrame timing out means nothing changed,
// while a frame update (or the cursor moving onto that screen) counts as
// activity. After IdleSeconds of stillness a transparent, click-through black
// overlay fades over that monitor down to Level% brightness, restoring quickly
// when activity resumes. With ContentAware on, change is split by the active
// window's footprint: when only that window is alive its surroundings dim while
// it stays carved out and bright, so the chrome rests without darkening what you
// are watching. A tray icon and a global hotkey toggle the whole behaviour at
// runtime, each with a balloon + beep so the change is felt. Knobs come from the
// [Dimmer] section of the dvhdr.ini beside the loader.
// ===========================================================================

#define DVHDR_DIM_TRAYMSG   (WM_APP + 1)
#define DVHDR_DIM_ID_TOGGLE 1001
#define DVHDR_DIM_ID_EXIT   1002
#define DVHDR_DIM_ID_FORCE  1003

static const wchar_t* kDimClass     = L"DVHDR_DimOverlay";
static const wchar_t* kDimMutexName = L"Local\\DVHDR-DWM-Dimmer";
static const wchar_t* kDimStopName  = L"Local\\DVHDR-DWM-Dimmer-Stop";

static const int  kDimGX        = 96;    // content-sample grid: points across
static const int  kDimGY        = 54;    //                      points down
static const int  kDimSampleTol = 6;     // per-point value delta that counts as "changed"
static const DWORD kDimCheckMs  = 200;   // min interval between pixel comparisons
static const DWORD kDimSettleMs = 400;   // ignore content changes until alpha is this stable
static const DWORD kDimTopoSettleMs = 750; // quiet period after the last WM_DISPLAYCHANGE before rebuilding
static const DWORD kDimRaiseMs  = 500;   // how often a visible overlay re-pins itself above the taskbar

// Excludes the overlay from screen capture (incl. Desktop Duplication) so its own
// fade isn't seen as a content change. Defined since Win10 2004; provide a fallback.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

struct DimmerCfg
{
    double idleSeconds     = 30.0;
    double level           = 25.0;   // target brightness %, 0..100
    double fadeSeconds     = 3.0;    // dim-down duration
    double wakeFadeSeconds = 0.3;    // wake-up duration
    double activityPct     = 0.1;    // min screen-% change that counts as activity
    bool   ignoreTaskbar   = true;   // discount changes inside the taskbar
    bool   contentAware    = true;   // keep the active window lit when only it is changing
    bool   debug           = false;  // write interruptions to a diagnostic log
    bool   startEnabled    = true;
    UINT   hotMods         = 0;      // MOD_* — 0 means no hotkey bound
    UINT   hotVk           = 0;
};

// One watched screen: its geometry + GDI device name (the join key to a DXGI
// output), its overlay window, and the duplication + owning-adapter device that
// feed its change signal. Pointers are owned by the vector element; DimFreeDup /
// DimDestroyMons release them.
struct DimMon
{
    int          index = 0;
    RECT         rect = {};
    std::wstring deviceName;
    HWND         hwnd = NULL;
    ID3D11Device*           dev = NULL;
    IDXGIOutputDuplication* dup = NULL;
    HRESULT      lastDupHr    = S_OK;    // last DuplicateOutput(1) result — diagnostic
    bool         primed       = false;   // first frame after (re)create is discarded
    ULONGLONG    lastActivity = 0;
    POINT        lastPtr      = {};
    bool         lastPtrValid = false;
    ULONGLONG    nextRetry    = 0;       // backoff before re-creating a lost dup
    double       curAlpha     = 0.0;     // 0..255, the animated overlay opacity
    ULONGLONG    lastAlphaChange = 0;    // when curAlpha last moved (self-change guard)
    bool         shown        = false;
    ID3D11DeviceContext* ctx  = NULL;    // immediate context of dev (copy + map)
    ID3D11Texture2D*     staging = NULL; // CPU-readable copy of the captured frame
    UINT         stageW = 0, stageH = 0;
    DXGI_FORMAT  stageFmt = DXGI_FORMAT_UNKNOWN;
    std::vector<unsigned char> prevSig;  // previous frame's grid signature
    ULONGLONG    lastCheck   = 0;        // throttle for the pixel comparison
    RECT         taskbarLocal = {};      // taskbar region on this screen, output-local
    bool         hasTaskbar   = false;
    int          lastHitCount = 0;       // grid points changed at the last interruption
    RECT         lastHitBox   = {};      // their bounding box, output-local
    // Content-aware dimming, tracked PER-MONITOR and sticky: this screen's own
    // content window (carved out and kept lit), independent of which screen holds
    // the global foreground. lastContentActivity is the timer for change inside it,
    // kept apart from lastActivity (change in the surroundings, which alone wakes).
    HWND         activeWnd    = NULL;    // this screen's remembered content window
    ULONGLONG    lastContentActivity = 0;
    RECT         activeWinLocal = {};    // its full window rect, output-local (its own zone — never wakes us)
    RECT         activeLocal  = {};      // its client rect, output-local (the bright hole)
    bool         hasActive    = false;   // a valid content window currently sits on this screen
    bool         rgnHasHole   = false;   // whether SetWindowRgn currently cuts a hole
    RECT         rgnHole      = {};      // the hole last applied (avoids redundant re-cuts)
    ULONGLONG    lastRaise    = 0;       // last time the visible overlay re-pinned itself top-most
    // The carve-out is filled by a second, window-confined shroud whose opacity is a
    // fraction of the surround's. Easing that fraction (holeLit: 0 = matches the
    // surround, 1 = fully clear) is what lets the window region fade in and out of
    // the dim instead of snapping when the rest of the screen is already shadowed.
    HWND         holeWnd      = NULL;    // patch shroud confined to the active window
    double       holeLit      = 0.0;     // 0..1 clarity of the patch (0 = matches surround)
    RECT         holePos      = {};      // patch geometry in screen coords (skip redundant moves)
};

static DimmerCfg           g_dimCfg;
static std::vector<DimMon> g_dimMons;
static std::vector<int>    g_dimReq;          // display numbers originally requested
static bool                g_dimEnabled = true;
static bool                g_dimForce   = false;     // constant dimming, ignores activity
static bool                g_dimQuit    = false;
static ULONGLONG           g_dimRebuildAt = 0;       // GetTickCount64 when a settled-topology rebuild is due (0 = none)
static HWND                g_dimCtrl    = NULL;
static NOTIFYICONDATAW     g_dimNid     = {};
static FILE*               g_dimLog     = NULL;
static char                g_dimLogPath[MAX_PATH] = {};

// Timestamped line to the diagnostic log (no-op unless [Dimmer] Debug = 1).
static void DimLog(const char* fmt, ...)
{
    if (!g_dimLog) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_dimLog, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_dimLog, fmt, ap);
    va_end(ap);
    fputc('\n', g_dimLog);
    fflush(g_dimLog);
}

// Open the diagnostic log beside the loader, falling back to %TEMP%. Stores the
// resolved path in g_dimLogPath so the startup balloon can point the user to it.
static void DimOpenLog()
{
    if (!g_dimCfg.debug) return;
    if (GetSiblingPath("dvhdr-dimmer.log", g_dimLogPath, sizeof(g_dimLogPath)))
        g_dimLog = fopen(g_dimLogPath, "w");
    if (!g_dimLog)
    {
        char tmp[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, tmp);
        if (n && n < MAX_PATH)
        {
            snprintf(g_dimLogPath, sizeof(g_dimLogPath), "%sdvhdr-dimmer.log", tmp);
            g_dimLog = fopen(g_dimLogPath, "w");
        }
    }
    if (!g_dimLog) g_dimLogPath[0] = '\0';
    DimLog("dimmer log opened — IdleSeconds=%.0f ActivityThreshold=%.3f%% IgnoreTaskbar=%d ContentAware=%d",
           g_dimCfg.idleSeconds, g_dimCfg.activityPct, g_dimCfg.ignoreTaskbar ? 1 : 0,
           g_dimCfg.contentAware ? 1 : 0);
}

// ---- hotkey string ("Ctrl+Alt+Shift+D") -> MOD_* mask + virtual key ----
static UINT DimKeyTokenToVk(const std::string& t)
{
    if (t.size() == 1)
    {
        char c = t[0];
        if (c >= 'a' && c <= 'z') return (UINT)(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return (UINT)c;
        if (c >= '0' && c <= '9') return (UINT)c;
    }
    if (!t.empty() && t[0] == 'f' && t.size() >= 2)
    {
        int n = atoi(t.c_str() + 1);
        if (n >= 1 && n <= 24) return (UINT)(VK_F1 + (n - 1));
    }
    if (t == "pause")    return VK_PAUSE;
    if (t == "space")    return VK_SPACE;
    if (t == "insert")   return VK_INSERT;
    if (t == "delete")   return VK_DELETE;
    if (t == "home")     return VK_HOME;
    if (t == "end")      return VK_END;
    if (t == "pageup")   return VK_PRIOR;
    if (t == "pagedown") return VK_NEXT;
    return 0;
}

static bool DimParseHotkey(const char* s, UINT& mods, UINT& vk)
{
    mods = 0; vk = 0;
    if (!s) return false;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty()) return;
        std::string l;
        for (char c : cur) l += (char)tolower((unsigned char)c);
        if      (l == "ctrl" || l == "control")                 mods |= MOD_CONTROL;
        else if (l == "alt")                                    mods |= MOD_ALT;
        else if (l == "shift")                                  mods |= MOD_SHIFT;
        else if (l == "win" || l == "windows" || l == "super")  mods |= MOD_WIN;
        else { UINT k = DimKeyTokenToVk(l); if (k) vk = k; }
        cur.clear();
    };
    for (const char* p = s; *p; p++)
    {
        if (*p == '+' || *p == ' ' || *p == '\t') flush();
        else cur += *p;
    }
    flush();
    return vk != 0;
}

static void LoadDimmerCfg(DimmerCfg& c)
{
    char ini[MAX_PATH];
    bool have = GetSiblingPath("dvhdr.ini", ini, sizeof(ini))
             && GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    auto F = [&](const char* k, double d) -> double {
        if (!have) return d;
        char buf[64];
        GetPrivateProfileStringA("Dimmer", k, "", buf, sizeof(buf), ini);
        return buf[0] ? atof(buf) : d;
    };
    c.idleSeconds     = F("IdleSeconds",     30.0);
    c.level           = F("Level",           25.0);
    c.fadeSeconds     = F("FadeSeconds",      3.0);
    c.wakeFadeSeconds = F("WakeFadeSeconds",  0.3);
    c.activityPct     = F("ActivityThreshold", 0.1);
    c.ignoreTaskbar   = have ? (GetPrivateProfileIntA("Dimmer", "IgnoreTaskbar", 1, ini) != 0) : true;
    c.contentAware    = have ? (GetPrivateProfileIntA("Dimmer", "ContentAware", 1, ini) != 0) : true;
    c.debug           = have ? (GetPrivateProfileIntA("Dimmer", "Debug", 0, ini) != 0) : false;
    c.startEnabled    = have ? (GetPrivateProfileIntA("Dimmer", "StartEnabled", 1, ini) != 0) : true;

    char hk[128];
    if (have) GetPrivateProfileStringA("Dimmer", "ToggleHotkey", "Ctrl+Alt+Shift+D", hk, sizeof(hk), ini);
    else      strcpy(hk, "Ctrl+Alt+Shift+D");
    DimParseHotkey(hk, c.hotMods, c.hotVk);

    if (c.level < 0)              c.level = 0;
    if (c.level > 100)            c.level = 100;
    if (c.idleSeconds < 1)        c.idleSeconds = 1;
    if (c.fadeSeconds < 0.05)     c.fadeSeconds = 0.05;
    if (c.wakeFadeSeconds < 0.05) c.wakeFadeSeconds = 0.05;
}

// ---- Desktop Duplication: create on the adapter that owns the output ----
static void DimFreeDup(DimMon& m)
{
    if (m.staging) { m.staging->Release(); m.staging = NULL; }
    if (m.dup) { m.dup->Release(); m.dup = NULL; }
    if (m.ctx) { m.ctx->Release(); m.ctx = NULL; }
    if (m.dev) { m.dev->Release(); m.dev = NULL; }
    m.primed = false;
    m.stageW = m.stageH = 0;
    m.stageFmt = DXGI_FORMAT_UNKNOWN;
    m.prevSig.clear();
}

static bool DimCreateDup(DimMon& m)
{
    IDXGIFactory1* factory = NULL;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;

    bool ok = false;
    for (UINT ai = 0; !ok; ai++)
    {
        // Stop on the first non-S_OK result (NOT_FOUND, or any transient failure
        // while the topology is mid-transition) and never enter the body with a
        // null adapter — EnumAdapters1 can hand one back during a detach.
        IDXGIAdapter1* adapter = NULL;
        if (factory->EnumAdapters1(ai, &adapter) != S_OK || !adapter) break;
        for (UINT oi = 0; !ok; oi++)
        {
            IDXGIOutput* output = NULL;
            if (adapter->EnumOutputs(oi, &output) != S_OK || !output) break;
            DXGI_OUTPUT_DESC od = {};
            if (SUCCEEDED(output->GetDesc(&od)) && m.deviceName == od.DeviceName)
            {
                // Explicit adapter REQUIRES D3D_DRIVER_TYPE_UNKNOWN (else E_INVALIDARG).
                ID3D11Device*        dev = NULL;
                ID3D11DeviceContext* ctx = NULL;
                D3D_FEATURE_LEVEL    fl;
                if (SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                                NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx)))
                {
                    IDXGIOutputDuplication* dup = NULL;
                    HRESULT dhr = E_FAIL;

                    // HDR / wide-gamut desktops: the legacy IDXGIOutput1::DuplicateOutput
                    // fails with DXGI_ERROR_UNSUPPORTED because it can't represent the
                    // FP16/HDR10 desktop surface. IDXGIOutput5::DuplicateOutput1 takes an
                    // explicit format list that includes those surfaces — try it first.
                    IDXGIOutput5* out5 = NULL;
                    if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput5), (void**)&out5)))
                    {
                        const DXGI_FORMAT fmts[] = {
                            DXGI_FORMAT_R16G16B16A16_FLOAT,   // scRGB HDR
                            DXGI_FORMAT_R10G10B10A2_UNORM,    // HDR10
                            DXGI_FORMAT_B8G8R8A8_UNORM,       // SDR
                        };
                        dhr = out5->DuplicateOutput1(dev, 0, ARRAYSIZE(fmts), fmts, &dup);
                        out5->Release();
                    }
                    if (FAILED(dhr))   // pre-1803, or Output5 unavailable — legacy path
                    {
                        IDXGIOutput1* out1 = NULL;
                        if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1)))
                        {
                            dhr = out1->DuplicateOutput(dev, &dup);
                            out1->Release();
                        }
                    }

                    m.lastDupHr = dhr;
                    if (SUCCEEDED(dhr) && dup)
                    {
                        m.dev = dev; dev = NULL;   // ownership moves to the monitor
                        m.ctx = ctx; ctx = NULL;   // kept for CopyResource + Map
                        m.dup = dup;
                        m.primed = false;
                        ok = true;
                    }
                    if (ctx) ctx->Release();
                    if (dev) dev->Release();
                }
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return ok;
}

// ---- real-pixel change detection ----
// Many GPU drivers report the whole screen as "dirty" on every present, so the
// duplication's move/dirty-rect metadata is useless for telling a caret blink
// from real motion. Instead we sample a sparse grid of ACTUAL pixels and compare
// it to the previous sample; only genuinely different pixels count.
static UINT64 DimIntersectArea(const RECT& a, const RECT& b)
{
    LONG l = a.left   > b.left   ? a.left   : b.left;
    LONG t = a.top    > b.top    ? a.top    : b.top;
    LONG r = a.right  < b.right  ? a.right  : b.right;
    LONG bo = a.bottom < b.bottom ? a.bottom : b.bottom;
    if (r <= l || bo <= t) return 0;
    return (UINT64)(r - l) * (UINT64)(bo - t);
}

static void DimUnion(RECT& u, const RECT& r)
{
    if (r.right <= r.left || r.bottom <= r.top) return;
    if (u.right <= u.left || u.bottom <= u.top) { u = r; return; }   // u empty -> set
    if (r.left   < u.left)   u.left   = r.left;
    if (r.top    < u.top)    u.top    = r.top;
    if (r.right  > u.right)  u.right  = r.right;
    if (r.bottom > u.bottom) u.bottom = r.bottom;
}

static UINT DimBytesPerPixel(DXGI_FORMAT f)
{
    return (f == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8u : 4u;  // else 8-bpc / R10G10B10A2
}

// The desktop and the taskbars are not content windows — focusing them must not
// carve a screen-sized hole, it should let the whole screen dim normally.
static bool DimIsShellWindow(HWND h)
{
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, ARRAYSIZE(cls))) return false;
    return !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW")
        || !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"Shell_SecondaryTrayWnd");
}

// Resolve THIS monitor's content window — the one carved out and kept lit while
// its surroundings dim. Tracked per-monitor and STICKY: the global foreground
// window updates only the monitor whose area holds its centre, so focusing a
// window on one screen never disturbs another's carve-out. The remembered window
// persists until it is closed, minimised, or leaves this screen, at which point
// the screen reverts to a plain full-screen dimmer. Fills m.activeWinLocal (the
// full window rect — this window's own zone, which must never wake the periphery)
// and m.activeLocal (the client rect — the bright hole), both output-local.
static void DimComputeActive(DimMon& m)
{
    if (!g_dimCfg.contentAware) { m.hasActive = false; m.activeWnd = NULL; return; }

    // Adopt the foreground window only if its centre lies on this screen, so each
    // window belongs to exactly one monitor and the others keep what they had.
    HWND fg = GetForegroundWindow();
    if (fg && IsWindowVisible(fg) && !DimIsShellWindow(fg))
    {
        RECT wr;
        if (GetWindowRect(fg, &wr))
        {
            POINT c = { (wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2 };
            if (PtInRect(&m.rect, c)) m.activeWnd = fg;
        }
    }

    m.hasActive = false;
    HWND a = m.activeWnd;
    if (!a || !IsWindow(a) || !IsWindowVisible(a) || IsIconic(a)) { m.activeWnd = NULL; return; }

    RECT wr, wi;
    if (!GetWindowRect(a, &wr) || !IntersectRect(&wi, &wr, &m.rect)) return;  // gone from this screen
    if (wi.right - wi.left < 8 || wi.bottom - wi.top < 8) return;
    m.activeWinLocal = { wi.left - m.rect.left, wi.top - m.rect.top,
                         wi.right - m.rect.left, wi.bottom - m.rect.top };

    // The bright hole is the client rect; fall back to the window rect if it can't
    // be resolved, so a window with no usable client area still gets carved out.
    RECT hole = wi, cr;
    if (GetClientRect(a, &cr))
    {
        POINT tl = { cr.left, cr.top }, br = { cr.right, cr.bottom };
        ClientToScreen(a, &tl);
        ClientToScreen(a, &br);
        RECT cs = { tl.x, tl.y, br.x, br.y }, ci;
        if (IntersectRect(&ci, &cs, &m.rect)) hole = ci;
    }
    m.activeLocal = { hole.left - m.rect.left, hole.top - m.rect.top,
                      hole.right - m.rect.left, hole.bottom - m.rect.top };
    m.hasActive = true;
}

// Copy the captured frame to a CPU-readable staging texture, sample a kDimGX x
// kDimGY grid of real pixels, and count how many differ from the previous sample
// by more than kDimSampleTol (skipping the taskbar when IgnoreTaskbar is set).
// The count is split: points falling INSIDE the active window's client rect feed
// *outInside, the rest feed *outOutside, so the caller can tell "only the active
// window is alive" (dim its surroundings) from "the periphery moved" (wake).
// *box is the bounding box of the OUTSIDE changes (output-local) — what counts as
// a wake. Both counts are 0 when it cannot read (a transient failure must not pin
// the screen awake) or on the first sample after a (re)create (baseline only).
static void DimFrameChanged(DimMon& m, IDXGIResource* res, RECT* box,
                            int* outInside, int* outOutside)
{
    if (box) *box = RECT{ 0, 0, 0, 0 };
    if (outInside)  *outInside  = 0;
    if (outOutside) *outOutside = 0;
    if (!m.ctx) return;

    ID3D11Texture2D* tex = NULL;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) || !tex) return;
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);

    if (!m.staging || m.stageW != d.Width || m.stageH != d.Height || m.stageFmt != d.Format)
    {
        if (m.staging) { m.staging->Release(); m.staging = NULL; }
        D3D11_TEXTURE2D_DESC s = {};
        s.Width = d.Width; s.Height = d.Height; s.MipLevels = 1; s.ArraySize = 1;
        s.Format = d.Format; s.SampleDesc.Count = 1;
        s.Usage = D3D11_USAGE_STAGING; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m.dev->CreateTexture2D(&s, NULL, &m.staging))) { tex->Release(); return; }
        m.stageW = d.Width; m.stageH = d.Height; m.stageFmt = d.Format;
        m.prevSig.clear();
    }

    m.ctx->CopyResource(m.staging, tex);
    tex->Release();

    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m.ctx->Map(m.staging, 0, D3D11_MAP_READ, 0, &map))) return;

    const UINT bpp = DimBytesPerPixel(d.Format);
    std::vector<unsigned char> sig((size_t)kDimGX * kDimGY);
    const BYTE* basePtr = (const BYTE*)map.pData;
    for (int gy = 0; gy < kDimGY; gy++)
    {
        UINT y = (UINT)(((double)gy + 0.5) * d.Height / kDimGY);
        const BYTE* rowp = basePtr + (size_t)y * map.RowPitch;
        for (int gx = 0; gx < kDimGX; gx++)
        {
            UINT x = (UINT)(((double)gx + 0.5) * d.Width / kDimGX);
            const BYTE* px = rowp + (size_t)x * bpp;
            sig[(size_t)gy * kDimGX + gx] =
                (unsigned char)(px[0] + px[1] * 3u + px[2] * 7u + px[3] * 11u);
        }
    }
    m.ctx->Unmap(m.staging, 0);

    int inside = 0, outside = 0;
    RECT bb = {};
    if (m.prevSig.size() == sig.size())
    {
        for (int gy = 0; gy < kDimGY; gy++)
            for (int gx = 0; gx < kDimGX; gx++)
            {
                size_t i = (size_t)gy * kDimGX + gx;
                int dv = (int)sig[i] - (int)m.prevSig[i];
                if (dv < 0) dv = -dv;
                if (dv <= kDimSampleTol) continue;

                LONG px = (LONG)(((double)gx + 0.5) * (LONG)(m.rect.right - m.rect.left) / kDimGX);
                LONG py = (LONG)(((double)gy + 0.5) * (LONG)(m.rect.bottom - m.rect.top) / kDimGY);
                if (g_dimCfg.ignoreTaskbar && m.hasTaskbar
                    && px >= m.taskbarLocal.left && px < m.taskbarLocal.right
                    && py >= m.taskbarLocal.top  && py < m.taskbarLocal.bottom) continue;

                if (m.hasActive
                    && px >= m.activeWinLocal.left && px < m.activeWinLocal.right
                    && py >= m.activeWinLocal.top  && py < m.activeWinLocal.bottom)
                {
                    inside++;   // this window's own frame (incl. title bar) — never a wake
                    continue;
                }

                outside++;
                RECT pr = { px, py, px + 1, py + 1 };
                DimUnion(bb, pr);
            }
    }
    m.prevSig.swap(sig);
    if (box) *box = bb;
    if (outInside)  *outInside  = inside;
    if (outOutside) *outOutside = outside;
}

// Poll one monitor's change signal. Never blocks (timeout 0). Recreates a lost
// duplication on a backoff; seeds activity to "now" on (re)create so recovery
// and startup don't false-wake.
static void DimPoll(DimMon& m, ULONGLONG now)
{
    if (g_dimRebuildAt) return;   // topology in flux — touch no DXGI until it settles
    if (!m.dup)
    {
        if (now >= m.nextRetry)
        {
            if (DimCreateDup(m)) { m.lastActivity = m.lastContentActivity = now; m.lastPtrValid = false; }
            else                 { m.nextRetry = now + 1000; }
        }
        return;
    }

    DXGI_OUTDUPL_FRAME_INFO fi = {};
    IDXGIResource* res = NULL;
    HRESULT hr = m.dup->AcquireNextFrame(0, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return;   // nothing changed; no frame held
    if (FAILED(hr))                               // ACCESS_LOST / DENIED / unavailable
    {
        DimFreeDup(m);
        m.nextRetry = now + 500;
        return;
    }

    if (!m.primed)
    {
        m.primed = true;            // discard the initial desktop frame
        m.lastPtrValid = false;
    }
    else
    {
        bool activity = false;
        // Real-pixel content check, throttled — a blinking caret, a ticking clock
        // or an animated icon changes too few grid points to clear the threshold,
        // so it no longer perpetually resets the idle timer (and it is immune to
        // drivers that mark the whole screen dirty every present). The active
        // window's footprint is resolved first so the change can be split: motion
        // INSIDE it keeps that window lit, motion OUTSIDE it is a true wake.
        if (fi.LastPresentTime.QuadPart != 0 && res && now - m.lastCheck >= kDimCheckMs)
        {
            m.lastCheck = now;
            DimComputeActive(m);
            RECT box = {};
            int  inside = 0, outside = 0;
            DimFrameChanged(m, res, &box, &inside, &outside);   // also refreshes the baseline
            int  total = kDimGX * kDimGY;
            int  thr   = (int)(g_dimCfg.activityPct / 100.0 * total);
            if (thr < 6) thr = 6;

            // Content inside the active window is read through the hole, which is
            // excluded from capture — so it is never our own fade and needs no settle
            // gate; refreshing it even mid-fade keeps the hole from blinking shut.
            if (inside > thr) m.lastContentActivity = now;   // active window alive → hold its hole open

            // The OUTSIDE wake stays gated: a frame captured while our opacity is
            // still moving differs from the last only because of us, so counting it
            // would cancel the very dim it is performing.
            bool settled = (now - m.lastAlphaChange) >= kDimSettleMs;
            if (settled && outside > thr)
            {
                activity = true;
                m.lastHitCount = outside;
                m.lastHitBox   = box;
                DimLog("D%d woke: %d/%d points changed outside active win, box (%ld,%ld)-(%ld,%ld)"
                       " [idle was %us]",
                       m.index, outside, total,
                       box.left, box.top, box.right, box.bottom,
                       (unsigned)((now - m.lastActivity) / 1000));
            }
        }
        if (fi.LastMouseUpdateTime.QuadPart != 0 && fi.PointerPosition.Visible)
        {
            // Cursor on THIS screen — count only genuine moves, not animated-cursor
            // shape churn (which also bumps LastMouseUpdateTime). A move over the
            // active window is content use (keep its hole alive); only a move over
            // the surroundings wakes the dim.
            POINT p = { fi.PointerPosition.Position.x, fi.PointerPosition.Position.y };
            if (!m.lastPtrValid || p.x != m.lastPtr.x || p.y != m.lastPtr.y)
            {
                bool overActive = m.hasActive
                    && p.x >= m.activeWinLocal.left && p.x < m.activeWinLocal.right
                    && p.y >= m.activeWinLocal.top  && p.y < m.activeWinLocal.bottom;
                if (overActive)
                    m.lastContentActivity = now;
                else
                {
                    if (!activity)
                        DimLog("D%d woke: cursor moved to (%ld,%ld)", m.index, p.x, p.y);
                    activity = true;
                }
            }
            m.lastPtr = p;
            m.lastPtrValid = true;
        }
        if (activity) m.lastActivity = now;
    }

    if (res) res->Release();
    m.dup->ReleaseFrame();
}

// ---- tray icon + toggle feedback ----
static void DimSetTrayTip()
{
    wsprintfW(g_dimNid.szTip, L"DVHDR idle-dim: %s", g_dimEnabled ? L"on" : L"off");
    g_dimNid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_dimNid);
}

// Live status in the tray tooltip — per monitor: "Dn dim" (overlay down),
// "Dn dim*" (down, but the active window is carved out and kept lit), "Dn 14s"
// (idle seconds counting toward the threshold), or "Dn no-capture" (Desktop
// Duplication unavailable, so it cannot be watched). Hover to read it.
static void DimStatusTip(ULONGLONG now)
{
    wchar_t buf[128];
    int off = wsprintfW(buf, L"DVHDR idle-dim: %s",
                        g_dimForce ? L"FORCED" : (g_dimEnabled ? L"on" : L"off"));
    for (auto& m : g_dimMons)
    {
        if (off > 110) break;
        if (!m.dup)
            off += wsprintfW(buf + off, L" | D%d no-capture", m.index);
        else if (m.curAlpha > 0.5)
            off += wsprintfW(buf + off, L" | D%d %s", m.index, m.rgnHasHole ? L"dim*" : L"dim");
        else
        {
            off += wsprintfW(buf + off, L" | D%d %us", m.index,
                             (unsigned)((now - m.lastActivity) / 1000));
            if (m.lastHitCount && off < 100)   // pin the last interruption so it's readable
                off += wsprintfW(buf + off, L" last %d@%ld,%ld",
                                 m.lastHitCount, m.lastHitBox.left, m.lastHitBox.top);
        }
    }
    lstrcpynW(g_dimNid.szTip, buf, ARRAYSIZE(g_dimNid.szTip));
    g_dimNid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_dimNid);
}

static void DimBalloon(const wchar_t* text)
{
    NOTIFYICONDATAW n = g_dimNid;
    n.uFlags = NIF_INFO;
    lstrcpynW(n.szInfo, text, ARRAYSIZE(n.szInfo));
    lstrcpynW(n.szInfoTitle, L"DVHDR-DWM", ARRAYSIZE(n.szInfoTitle));
    n.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void DimToggle()
{
    g_dimEnabled = !g_dimEnabled;
    ULONGLONG now = GetTickCount64();
    for (auto& m : g_dimMons) m.lastActivity = now;   // re-arm idle on either flip
    DimSetTrayTip();
    DimBalloon(g_dimEnabled ? L"Idle dimming enabled" : L"Idle dimming disabled");
    MessageBeep(g_dimEnabled ? MB_OK : MB_ICONASTERISK);
    DimLog("toggled %s", g_dimEnabled ? "ON" : "OFF");
}

// Constant dimming: hold the watched screens dark regardless of activity, until
// turned off again — "enforce the night". Independent of the idle toggle.
static void DimToggleForce()
{
    g_dimForce = !g_dimForce;
    ULONGLONG now = GetTickCount64();
    for (auto& m : g_dimMons) m.lastActivity = now;   // so it re-evaluates cleanly when lifted
    DimSetTrayTip();
    DimBalloon(g_dimForce ? L"Constant dimming ON — screen held dark until you turn it off"
                          : L"Constant dimming off");
    MessageBeep(g_dimForce ? MB_OK : MB_ICONASTERISK);
    DimLog("force dim %s", g_dimForce ? "ON" : "OFF");
}

static LRESULT CALLBACK DimWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_HOTKEY:
        DimToggle();
        return 0;
    case WM_DISPLAYCHANGE:                 // topology / resolution change
        // Sleep/wake walks the topology through several stages, each firing this
        // broadcast. Defer the rebuild until the changes fall quiet (and meanwhile
        // suspend all capture, below) rather than recreating DXGI devices in the
        // middle of a detach, which is when the duplication layer is unstable.
        g_dimRebuildAt = GetTickCount64() + kDimTopoSettleMs;
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        g_dimQuit = true;
        return 0;
    case DVHDR_DIM_TRAYMSG:
        if (h == g_dimCtrl)
        {
            switch (LOWORD(lp))
            {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                DimToggle();
                break;
            case WM_RBUTTONUP:
            {
                POINT pt; GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING | (g_dimEnabled ? MF_CHECKED : 0u),
                            DVHDR_DIM_ID_TOGGLE, L"Idle dimming");
                AppendMenuW(menu, MF_STRING | (g_dimForce ? MF_CHECKED : 0u),
                            DVHDR_DIM_ID_FORCE, L"Force dim (always on)");
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, MF_STRING, DVHDR_DIM_ID_EXIT, L"Exit");
                SetForegroundWindow(h);    // so the menu dismisses on click-away
                int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                              pt.x, pt.y, 0, h, NULL);
                DestroyMenu(menu);
                if      (cmd == DVHDR_DIM_ID_TOGGLE) DimToggle();
                else if (cmd == DVHDR_DIM_ID_FORCE)  DimToggleForce();
                else if (cmd == DVHDR_DIM_ID_EXIT)   g_dimQuit = true;
                break;
            }
            }
        }
        return 0;
    case WM_DESTROY:
        // Only the control window's destruction ends the program. Overlay windows
        // share this wndproc and are destroyed/recreated on every topology rebuild
        // (e.g. monitors detaching on sleep); without this guard an overlay's
        // WM_DESTROY would post WM_QUIT and silently kill the whole dimmer.
        if (h == g_dimCtrl) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void DimDestroyMons()
{
    for (auto& m : g_dimMons)
    {
        if (m.hwnd)    { DestroyWindow(m.hwnd);    m.hwnd = NULL; }
        if (m.holeWnd) { DestroyWindow(m.holeWnd); m.holeWnd = NULL; }
        DimFreeDup(m);
    }
    g_dimMons.clear();
}

// Locate the taskbar(s) and record, per watched monitor, the taskbar rectangle
// that sits on it (in that screen's output-local coordinates) so DimCountRect can
// discount it. Cheap; refreshed at build time and once a second.
static void DimRefreshTaskbars()
{
    std::vector<RECT> bars;
    HWND tb = FindWindowW(L"Shell_TrayWnd", NULL);            // primary taskbar
    if (tb) { RECT r; if (GetWindowRect(tb, &r)) bars.push_back(r); }
    HWND sec = NULL;                                          // one per secondary monitor
    while ((sec = FindWindowExW(NULL, sec, L"Shell_SecondaryTrayWnd", NULL)) != NULL)
    {
        RECT r; if (GetWindowRect(sec, &r)) bars.push_back(r);
    }
    for (auto& m : g_dimMons)
    {
        m.hasTaskbar = false;
        for (auto& b : bars)
        {
            if (DimIntersectArea(b, m.rect) == 0) continue;  // not on this screen
            RECT loc = { b.left - m.rect.left, b.top - m.rect.top,
                         b.right - m.rect.left, b.bottom - m.rect.top };
            LONG w = m.rect.right - m.rect.left, h = m.rect.bottom - m.rect.top;
            if (loc.left   < 0) loc.left   = 0;
            if (loc.top    < 0) loc.top    = 0;
            if (loc.right  > w) loc.right  = w;
            if (loc.bottom > h) loc.bottom = h;
            m.taskbarLocal = loc;
            m.hasTaskbar = true;
            break;
        }
    }
}

// Resolve the requested display numbers to monitors, creating an overlay and a
// duplication for each present one. Tolerant: an absent display is skipped (and
// picked up later if it returns, via WM_DISPLAYCHANGE -> rebuild).
static void DimBuildMons(HINSTANCE hinst)
{
    DimDestroyMons();
    auto all = EnumDisplays();
    for (int want : g_dimReq)
    {
        const DisplayInfo* found = NULL;
        for (auto& d : all) if (d.index == want) { found = &d; break; }
        if (!found) { msg("Display %d not attached — skipping", want); continue; }

        DimMon m;
        m.index        = found->index;
        m.rect.left    = found->left;
        m.rect.top     = found->top;
        m.rect.right   = found->left + found->width;
        m.rect.bottom  = found->top  + found->height;
        m.deviceName   = found->deviceName;
        m.lastActivity = m.lastContentActivity = GetTickCount64();

        m.hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kDimClass, L"", WS_POPUP,
            m.rect.left, m.rect.top, m.rect.right - m.rect.left, m.rect.bottom - m.rect.top,
            NULL, NULL, hinst, NULL);
        if (m.hwnd)
        {
            SetLayeredWindowAttributes(m.hwnd, 0, 0, LWA_ALPHA);
            // Keep the overlay out of Desktop Duplication so its own fade isn't
            // mistaken for desktop activity (best-effort; pre-2004 ignores it).
            SetWindowDisplayAffinity(m.hwnd, WDA_EXCLUDEFROMCAPTURE);
            // Map it now, fully transparent, and leave it mapped for life — only the
            // opacity animates. A hidden->shown transition at dim time composites one
            // unclipped frame, flashing the carved window dark before the region bites.
            ShowWindow(m.hwnd, SW_SHOWNOACTIVATE);
        }

        // The patch shroud that fills the carve-out and fades it in/out. Same style
        // as the surround and likewise excluded from capture, so its own fade is
        // never read back as content stirring inside the window (a feedback loop that
        // would keep the screen perpetually awake). Mapped for life at alpha 0
        // (invisible); DimTick moves it over the window and animates its opacity.
        m.holeWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kDimClass, L"", WS_POPUP,
            m.rect.left, m.rect.top, 1, 1,
            NULL, NULL, hinst, NULL);
        if (m.holeWnd)
        {
            SetLayeredWindowAttributes(m.holeWnd, 0, 0, LWA_ALPHA);
            SetWindowDisplayAffinity(m.holeWnd, WDA_EXCLUDEFROMCAPTURE);
            ShowWindow(m.holeWnd, SW_SHOWNOACTIVATE);
        }

        DimCreateDup(m);   // may fail right now; DimPoll retries on a backoff
        g_dimMons.push_back(m);
    }
    DimRefreshTaskbars();
}

// Carve the active window's client rect out of the overlay (or restore it whole)
// via the window region. SetWindowRgn takes ownership of the region handle, so a
// fresh one is built each time — but only when the hole's presence or rectangle
// actually changes, so a steady idle-dim costs nothing. The cut-out area is not
// part of the window, so the live content beneath shows at full luminance while
// the surrounding (in-region) pixels keep the overlay's animated dim alpha.
static void DimApplyHole(DimMon& m, bool wantHole)
{
    if (!m.hwnd) return;
    RECT hole = wantHole ? m.activeLocal : RECT{ 0, 0, 0, 0 };
    if (wantHole == m.rgnHasHole && (!wantHole || EqualRect(&hole, &m.rgnHole))) return;

    if (wantHole)
    {
        LONG w = m.rect.right - m.rect.left, h = m.rect.bottom - m.rect.top;
        HRGN rgn = CreateRectRgn(0, 0, w, h);
        HRGN cut = CreateRectRgn(hole.left, hole.top, hole.right, hole.bottom);
        CombineRgn(rgn, rgn, cut, RGN_DIFF);
        DeleteObject(cut);
        SetWindowRgn(m.hwnd, rgn, TRUE);   // window owns rgn from here
        DimLog("D%d content-hole open (%ld,%ld)-(%ld,%ld)",
               m.index, hole.left, hole.top, hole.right, hole.bottom);
    }
    else
    {
        SetWindowRgn(m.hwnd, NULL, TRUE);
        if (m.rgnHasHole) DimLog("D%d content-hole closed", m.index);
    }
    m.rgnHasHole = wantHole;
    m.rgnHole    = hole;
}

// Position the patch shroud over the carved hole (the active window's client rect,
// in screen coords). Only moves when the rectangle actually changes; its opacity is
// driven separately each tick. With no carve the patch is left where it sits —
// DimTick fades its alpha to zero, so it simply vanishes without a move.
static void DimApplyPatch(DimMon& m, bool carve)
{
    if (!m.holeWnd || !carve) return;
    RECT r = { m.rect.left + m.activeLocal.left,  m.rect.top + m.activeLocal.top,
               m.rect.left + m.activeLocal.right, m.rect.top + m.activeLocal.bottom };
    if (EqualRect(&r, &m.holePos)) return;
    SetWindowPos(m.holeWnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    m.holePos = r;
}

// Advance each overlay one tick toward its target opacity.
static void DimTick(ULONGLONG now, double dtMs)
{
    double dimAlpha = (1.0 - g_dimCfg.level / 100.0) * 255.0;
    if (dimAlpha < 0)   dimAlpha = 0;
    if (dimAlpha > 255) dimAlpha = 255;
    double fadeS = g_dimCfg.fadeSeconds     > 0.05 ? g_dimCfg.fadeSeconds     : 0.05;
    double wakeS = g_dimCfg.wakeFadeSeconds > 0.05 ? g_dimCfg.wakeFadeSeconds : 0.05;

    for (auto& m : g_dimMons)
    {
        DimPoll(m, now);

        double idle      = (double)(now - m.lastActivity) / 1000.0;
        bool   watchable = (m.dup != NULL);
        // Force = always dark (no capture needed); otherwise the idle rule.
        double target    = (g_dimForce
                            || (g_dimEnabled && watchable && idle >= g_dimCfg.idleSeconds))
                           ? dimAlpha : 0.0;

        bool   dimming = target > m.curAlpha;
        double step    = (dimAlpha / (dimming ? fadeS : wakeS)) * (dtMs / 1000.0);
        if (step < 0) step = -step;
        double prevAlpha = m.curAlpha;
        if (dimming) m.curAlpha = (m.curAlpha + step >= target) ? target : m.curAlpha + step;
        else         m.curAlpha = (m.curAlpha - step <= target) ? target : m.curAlpha - step;
        if (m.curAlpha != prevAlpha) m.lastAlphaChange = now;   // arms the self-change guard

        if (m.hwnd)
        {
            // Content-aware: keep this screen's content window lit while it is alive
            // (change inside it within the idle window). Whenever a content window is
            // present we carve it out of the surround and fill the gap with the patch
            // shroud. The patch's dim is a fraction of the surround's — holeLit eases
            // 0->1 as the window goes lit — so the carve-out melts in and out of the
            // shadow instead of snapping when the surroundings are already dimmed. At
            // holeLit 0 the patch exactly equals the surround, so a plain idle screen
            // still settles as one seamless sheet. Force-dim stays a full blackout.
            double contentIdle = (double)(now - m.lastContentActivity) / 1000.0;
            bool   carve   = g_dimCfg.contentAware && !g_dimForce && m.hasActive;
            bool   wantLit = carve && contentIdle < g_dimCfg.idleSeconds;

            double litTarget = wantLit ? 1.0 : 0.0;
            double litDur    = wantLit ? wakeS : fadeS;   // brighten snappy, settle gently
            double litStep   = (1.0 / litDur) * (dtMs / 1000.0);
            if (m.holeLit < litTarget) m.holeLit = (m.holeLit + litStep >= litTarget) ? litTarget : m.holeLit + litStep;
            else                       m.holeLit = (m.holeLit - litStep <= litTarget) ? litTarget : m.holeLit - litStep;

            DimApplyPatch(m, carve);
            DimApplyHole(m, carve);

            if (m.holeWnd)
            {
                double holeAlpha = carve ? m.curAlpha * (1.0 - m.holeLit) : 0.0;
                SetLayeredWindowAttributes(m.holeWnd, 0, (BYTE)(holeAlpha + 0.5), LWA_ALPHA);
            }
            SetLayeredWindowAttributes(m.hwnd, 0, (BYTE)(m.curAlpha + 0.5), LWA_ALPHA);
            // The overlay stays mapped; only its opacity moves. m.shown now tracks
            // the logical dim state for the log, not the window's visibility.
            bool want = m.curAlpha > 0.5;
            bool justShown = want && !m.shown;
            if      (justShown)        { m.shown = true;  DimLog("D%d begins dimming (idle %.0fs)", m.index, idle); }
            else if (!want && m.shown) { m.shown = false; DimLog("D%d fully cleared", m.index); }

            // Re-pin above the per-monitor taskbar, which re-asserts its own top-most
            // z-order on focus changes and would otherwise float its strip above the
            // shroud. Done without activating (no flash); on the dim's onset and then
            // throttled, so we don't wrestle the shell every frame. The carve-out hole
            // keeps the active window lit even with the overlay above it.
            if (want && (justShown || now - m.lastRaise >= kDimRaiseMs))
            {
                SetWindowPos(m.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                // Pin the patch last so it rides just above the surround, leaving no
                // seam at the carve-out's edge when both are dimmed.
                if (m.holeWnd && carve)
                    SetWindowPos(m.holeWnd, HWND_TOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                m.lastRaise = now;
            }
        }
    }
}

static int RunDimmer(const std::vector<int>& indices)
{
    g_dimReq = indices;

    HANDLE mutex = CreateMutexW(NULL, FALSE, kDimMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        msg("dimmer already running");
        CloseHandle(mutex);
        return 0;
    }
    HANDLE stopEvt = CreateEventW(NULL, TRUE, FALSE, kDimStopName);

    LoadDimmerCfg(g_dimCfg);
    g_dimEnabled = g_dimCfg.startEnabled;

    HINSTANCE hinst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DimWndProc;
    wc.hInstance     = hinst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kDimClass;
    RegisterClassExW(&wc);

    // Hidden top-level control window: owns the tray icon + hotkey and (being
    // top-level, not message-only) receives the WM_DISPLAYCHANGE broadcast.
    g_dimCtrl = CreateWindowExW(WS_EX_TOOLWINDOW, kDimClass, L"DVHDR Dimmer",
                                WS_POPUP, 0, 0, 0, 0, NULL, NULL, hinst, NULL);

    g_dimNid = {};
    g_dimNid.cbSize           = sizeof(g_dimNid);
    g_dimNid.hWnd             = g_dimCtrl;
    g_dimNid.uID              = 1;
    g_dimNid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_dimNid.uCallbackMessage = DVHDR_DIM_TRAYMSG;
    g_dimNid.hIcon = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_dimNid.hIcon) g_dimNid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wsprintfW(g_dimNid.szTip, L"DVHDR idle-dim: %s", g_dimEnabled ? L"on" : L"off");
    Shell_NotifyIconW(NIM_ADD, &g_dimNid);

    bool hotOk = false;
    if (g_dimCfg.hotVk)
        hotOk = RegisterHotKey(g_dimCtrl, 1, g_dimCfg.hotMods | MOD_NOREPEAT, g_dimCfg.hotVk) != 0;

    DimBuildMons(hinst);
    DimOpenLog();

    // Startup summary — tell the user up front whether each screen can actually be
    // captured (Desktop Duplication can fail on HDR via the legacy path, when
    // another grabber holds the output, etc.). Without capture, nothing can dim.
    {
        int okN = 0; HRESULT failHr = S_OK; int failIdx = 0;
        for (auto& m : g_dimMons)
            if (m.dup) okN++;
            else { failHr = m.lastDupHr; failIdx = m.index; }
        wchar_t b[400];
        if (okN == (int)g_dimMons.size() && okN > 0)
            wsprintfW(b, L"Watching %d display(s) — idle-dim %s", okN, g_dimEnabled ? L"on" : L"off");
        else
            wsprintfW(b, L"Display %d cannot be captured (0x%08X) — it will not dim. "
                         L"Close any other screen-capture tool and retry.", failIdx, (unsigned)failHr);
        if (g_dimLog && g_dimLogPath[0])
        {
            wchar_t wpath[MAX_PATH] = {};
            MultiByteToWideChar(CP_ACP, 0, g_dimLogPath, -1, wpath, MAX_PATH);
            wchar_t b2[200]; lstrcpynW(b2, b, ARRAYSIZE(b2));
            wsprintfW(b, L"%s\nLogging interruptions to %s", b2, wpath);
        }
        DimBalloon(b);
    }

    ULONGLONG last    = GetTickCount64();
    ULONGLONG lastTip = 0;
    const DWORD frameMs = 16;
    bool running = true;
    while (running)
    {
        DWORD w = MsgWaitForMultipleObjectsEx(1, &stopEvt, frameMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (w == WAIT_OBJECT_0) break;   // --dim-stop signalled

        MSG mm;
        while (PeekMessageW(&mm, NULL, 0, 0, PM_REMOVE))
        {
            if (mm.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&mm);
            DispatchMessageW(&mm);
        }
        if (!running || g_dimQuit) break;

        ULONGLONG now = GetTickCount64();

        if (g_dimRebuildAt)
        {
            // A topology change is underway: release every duplication at once so no
            // capture call lands on an output being detached, and hold off the
            // rebuild until the broadcasts have been silent for kDimTopoSettleMs.
            for (auto& m : g_dimMons) DimFreeDup(m);
            if (now >= g_dimRebuildAt) { g_dimRebuildAt = 0; DimBuildMons(hinst); }
        }

        double dtMs = (double)(now - last);
        last = now;
        if (dtMs <= 0)   dtMs = frameMs;
        if (dtMs > 250)  dtMs = 250;     // clamp long stalls so the fade can't jump
        DimTick(now, dtMs);

        if (now - lastTip >= 1000) { lastTip = now; DimRefreshTaskbars(); DimStatusTip(now); }
    }

    DimLog("dimmer stopping");
    DimDestroyMons();
    Shell_NotifyIconW(NIM_DELETE, &g_dimNid);
    if (hotOk)     UnregisterHotKey(g_dimCtrl, 1);
    if (g_dimCtrl) DestroyWindow(g_dimCtrl);
    UnregisterClassW(kDimClass, hinst);
    if (g_dimLog) { fclose(g_dimLog); g_dimLog = NULL; }
    if (stopEvt) CloseHandle(stopEvt);
    if (mutex)   CloseHandle(mutex);
    return 0;
}

static int RunDimStop()
{
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, kDimStopName);
    if (!e) { msg("no running dimmer found"); return 0; }
    SetEvent(e);
    CloseHandle(e);
    msg("dimmer stop signalled");
    return 0;
}

enum class Mode { Auto, Force, Unload, Status, List, Dim, DimStop };

int main(int argc, char** argv)
{
    AttachParentConsole();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Mode mode = Mode::Auto;
    std::vector<int> monitorIndices;
    std::vector<int> dimIndices;
    for (int i = 1; i < argc; i++)
    {
        if (!_stricmp(argv[i], "--force"))       mode = Mode::Force;
        else if (!_stricmp(argv[i], "--unload")) mode = Mode::Unload;
        else if (!_stricmp(argv[i], "--status")) mode = Mode::Status;
        else if (!_stricmp(argv[i], "--list"))   mode = Mode::List;
        else if (!_stricmp(argv[i], "--dim-stop")) mode = Mode::DimStop;
        else if (!_stricmp(argv[i], "-q") || !_stricmp(argv[i], "--silent")) g_silent = true;
        else if (!_stricmp(argv[i], "-m") || !_stricmp(argv[i], "--monitors"))
        {
            if (i + 1 >= argc) { fprintf(stderr, "%s expects a value\n", argv[i]); return 2; }
            char* spec = argv[++i];
            for (char* tok = strtok(spec, ","); tok; tok = strtok(NULL, ","))
            {
                int n = atoi(tok);
                if (n <= 0) { fprintf(stderr, "Invalid display number: %s\n", tok); return 2; }
                monitorIndices.push_back(n);
            }
        }
        else if (!_stricmp(argv[i], "--dim"))
        {
            if (i + 1 >= argc) { fprintf(stderr, "%s expects a value\n", argv[i]); return 2; }
            char* spec = argv[++i];
            for (char* tok = strtok(spec, ","); tok; tok = strtok(NULL, ","))
            {
                int n = atoi(tok);
                if (n <= 0) { fprintf(stderr, "Invalid display number: %s\n", tok); return 2; }
                dimIndices.push_back(n);
            }
            mode = Mode::Dim;
        }
        else if (!_stricmp(argv[i], "-h") || !_stricmp(argv[i], "--help"))
        {
            OpenOutputGap();
            printf("dvhdrloader [--force|--unload|--status|--list] [-m N[,N...]] [-q]\n");
            printf("            [--dim N[,N...]] [--dim-stop]\n");
            printf("  (none)        inject dvhdr.dll if absent, otherwise no-op\n");
            printf("  --force       unload + reinject (reloads config)\n");
            printf("  --unload      remove dvhdr.dll from dwm.exe\n");
            printf("  --status      report whether dvhdr.dll is currently loaded\n");
            printf("  --list        enumerate displays with index + coords\n");
            printf("  -m N[,N...]   tonemap these display number(s) (per-screen caps via [Display.N] in the ini), then force-reinject\n");
            printf("  --dim N[,N...] idle-dim these display number(s): watch each and fade it down\n");
            printf("                after [Dimmer] IdleSeconds of no change. Persistent; toggle with\n");
            printf("                the [Dimmer] ToggleHotkey or the tray icon. Runs the overlay, no injection.\n");
            printf("  --dim-stop    signal a running --dim watcher to exit\n");
            printf("  -q/--silent   suppress output (no console window appears regardless)\n");
            return 0;
        }
        else { fprintf(stderr, "Unknown arg: %s\n", argv[i]); return 2; }
    }

    if (mode == Mode::List)
    {
        PrintDisplays(EnumDisplays());
        return 0;
    }

    // The idle-dimmer is a self-contained user-session feature — it owns its own
    // overlay/watch loop and never touches the SYSTEM-impersonation / injection
    // path below, so it returns here.
    if (mode == Mode::DimStop) return RunDimStop();
    if (mode == Mode::Dim)     return RunDimmer(dimIndices);

    // -m resolves display numbers → registry, then implies --force so the
    // DLL picks up the new selection on its next attach.
    if (!monitorIndices.empty())
    {
        auto all = EnumDisplays();
        std::vector<DisplayInfo> picked;
        for (int want : monitorIndices)
        {
            bool found = false;
            for (auto& d : all) if (d.index == want) { picked.push_back(d); found = true; break; }
            if (!found) { msg("Display %d not attached", want); return 8; }
        }
        if (!WriteMonitorsToRegistry(picked))
        {
            msg("Could not write HKLM\\%ls\\%ls (%lu) — run elevated",
                DVHDR_REG_PATH, DVHDR_REG_VALUE, GetLastError());
            return 9;
        }
        msg("Configured %zu display(s) in registry", picked.size());
        if (mode == Mode::Auto) mode = Mode::Force; // ensure the DLL reloads
    }

    char sourceDll[MAX_PATH], installedDll[MAX_PATH];
    if (!GetSourceDllPath(sourceDll, sizeof(sourceDll)))
    {
        msg("dvhdr.dll not found next to loader");
        return 3;
    }
    if (!GetInstalledDllPath(installedDll, sizeof(installedDll)))
    {
        msg("Could not resolve %%SYSTEMROOT%%\\Temp\\dvhdr.dll");
        return 3;
    }

    if (!EnableDebugPrivilege())
    {
        msg("Could not enable SeDebugPrivilege — required to grab the SYSTEM token");
        return 5;
    }
    if (!ImpersonateSystem()) return 5;
    // From here on, the calling thread runs as SYSTEM. RevertToSelf before return.

    DWORD pid = FindDwmInCurrentSession();
    if (!pid) { msg("dwm.exe not found in current session"); RevertToSelf(); return 4; }

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
                            | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_CREATE_THREAD,
                              FALSE, pid);
    if (!proc) { msg("OpenProcess(dwm.exe) failed (%lu)", GetLastError()); RevertToSelf(); return 5; }

    HMODULE existing = FindLoadedModule(proc, "dvhdr.dll");

    int rc = 0;
    switch (mode)
    {
    case Mode::Status:
        msg(existing ? "loaded" : "not loaded");
        rc = existing ? 0 : 1;
        break;

    case Mode::Unload:
        if (!existing) { msg("not loaded — nothing to do"); break; }
        if (Unload(proc, existing)) { msg("unloaded"); RemovePayloadInstalled(); }
        else { msg("unload failed"); rc = 6; }
        break;

    case Mode::Force:
        if (existing && !Unload(proc, existing)) { msg("force: unload failed"); rc = 6; break; }
        if (!EnsurePayloadInstalled(NULL)) { rc = 10; break; }
        if (Inject(proc, installedDll)) msg("injected");
        else { msg("inject failed"); rc = 7; }
        break;

    case Mode::Auto:
        if (existing) { msg("already loaded"); break; }
        if (!EnsurePayloadInstalled(NULL)) { rc = 10; break; }
        if (Inject(proc, installedDll)) msg("injected");
        else { msg("inject failed"); rc = 7; }
        break;

    case Mode::List:
    case Mode::Dim:
    case Mode::DimStop:
        // Handled earlier (each returns before this switch); here only to silence -Wswitch.
        break;
    }

    RevertToSelf();

    CloseHandle(proc);
    return rc;
}
