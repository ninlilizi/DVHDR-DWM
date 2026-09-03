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
//   --guard           persistent, elevated watcher that unloads the shader while
//                     a game holds the foreground (restoring MPO / G-Sync) and
//                     re-injects when it exits. Same fullscreen + ETW present-rate
//                     detection as OLEDSaver. See the [Guard] ini section.
//   --guard-stop      stop a running --guard watcher and unload the shader
//                     (restores MPO / DirectFlip); same as the tray Exit item
//   -q/--silent       suppress console output

#include "pch.h"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")  // Shell_NotifyIcon - guard tray icon
#pragma comment(lib, "gdi32.lib")    // DIB compositing for the status dot

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

// Resident enable/disable channel. HKLM\SOFTWARE\DVHDR-DWM "Enabled" (REG_DWORD):
// 1 = render + MPO off, 0 = passthrough + MPO on. The injected shader polls this
// so the guard can pause/resume for games (and honour the tray toggle) WITHOUT
// unloading and re-injecting - the operation that races DWM and crashes it.
static void WriteEnabledToRegistry(bool enabled)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, DVHDR_REG_PATH, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return;
    DWORD val = enabled ? 1u : 0u;
    RegSetValueExW(key, L"Enabled", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    RegCloseKey(key);
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
// Game-aware injection guard - `dvhdrloader --guard`.
//
// A persistent, elevated user-session watcher that PAUSES the DWM shader while a
// game holds the foreground. The injected dvhdr.dll disables MPO / DirectFlip
// (globally on 25H2, via dwm's OverlayTestMode) so its Present hook can tonemap
// every composite - which is exactly what robs a borderless or windowed game of
// independent flip, and G-Sync with it. So when a game appears the guard UNLOADS
// the DLL, returning dwm.exe to its clean launch state with MPO re-enabled; when
// the game leaves, it re-injects. Detection is the same heuristic OLEDSaver uses:
// a geometric fullscreen/borderless test plus an ETW present-rate watch for
// windowed games, both debounced over two scans. Knobs live in [Guard] in the ini.
//
// Carries a tray icon whose status dot reads green while the shader is injected
// and tonemapping, red while it is unloaded - paused for a foreground game or
// manually disabled. Its menu (or a left-click) offers a manual enable / disable
// that overrides detection until toggled back. A message-pumping wait loop keeps
// that icon live alongside the polled detection
// (the ETW consumer runs on its own thread). Must run elevated - injecting into
// dwm.exe needs the SYSTEM token. Stop it with `--guard-stop` or the tray's Exit
// item: either is a full shutdown that unloads the shader (restoring MPO /
// DirectFlip) and removes the installed payload, so nothing is left injected
// without a watcher. A logoff just quits - dwm.exe is torn down regardless.
// ===========================================================================

struct GuardCfg
{
    bool   pauseOnFullscreen  = true;
    bool   pauseOnGamePresent = true;
    double gamePresentFps     = 20.0;
    bool   debug              = false;
};
static GuardCfg g_gCfg;
static FILE*    g_gLog = NULL;
static char     g_gLogPath[MAX_PATH] = {};

static const wchar_t* kGuardMutexName = L"Local\\DVHDR-DWM-Guard";
static const wchar_t* kGuardStopName  = L"Local\\DVHDR-DWM-Guard-Stop";
static const DWORD     kGuardScanMs        = 2000;   // detection cadence
static const DWORD     kGuardReinjectMs    = 15000;  // re-inject check while running (recovers a restarted dwm)

static void GLog(const char* fmt, ...)
{
    if (!g_gLog) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_gLog, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_gLog, fmt, ap);
    va_end(ap);
    fputc('\n', g_gLog);
    fflush(g_gLog);
}

// Open dvhdr-guard.log beside the loader (fallback %TEMP%) when [Guard] Debug = 1.
static void GOpenLog()
{
    if (!g_gCfg.debug) return;
    if (GetSiblingPath("dvhdr-guard.log", g_gLogPath, sizeof(g_gLogPath)))
        g_gLog = fopen(g_gLogPath, "w");
    if (!g_gLog)
    {
        char tmp[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, tmp);
        if (n && n < MAX_PATH)
        {
            snprintf(g_gLogPath, sizeof(g_gLogPath), "%sdvhdr-guard.log", tmp);
            g_gLog = fopen(g_gLogPath, "w");
        }
    }
    if (!g_gLog) g_gLogPath[0] = '\0';
}

static void LoadGuardCfg(GuardCfg& c)
{
    char ini[MAX_PATH];
    bool have = GetSiblingPath("dvhdr.ini", ini, sizeof(ini))
             && GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    auto B = [&](const char* k, int d) -> bool {
        return (have ? GetPrivateProfileIntA("Guard", k, d, ini) : d) != 0;
    };
    c.pauseOnFullscreen  = B("PauseOnFullscreen", 1);
    c.pauseOnGamePresent = B("PauseOnGamePresent", 1);
    c.debug              = B("Debug", 0);
    if (have)
    {
        char buf[64];
        GetPrivateProfileStringA("Guard", "GamePresentFps", "", buf, sizeof(buf), ini);
        if (buf[0]) c.gamePresentFps = atof(buf);
    }
}

// ---- ETW present-rate watch (PresentMon's wells, drunk shallowly) ----
// A real-time trace session on the Microsoft-Windows-DXGI and -D3D9 user-mode
// providers. Their Present_Start events fire in the presenting process, so the
// event header's ProcessId alone says who is pushing frames - no payload
// decoding, no DxgKrnl state machine. The consumer thread tallies presents per
// PID; each 2-second scan reads the foreground process's tally and clears the
// board, yielding its present rate. Needs elevation or membership in the
// Performance Log Users group; when denied the watch quietly stands aside and
// the geometric fullscreen test carries on alone.

static const wchar_t* kGuardEtwName = L"DVHDR-GuardPresentWatch";
static const GUID kGuardEtwDxgi = { 0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 } };
static const GUID kGuardEtwD3D9 = { 0x783ACA0A, 0x790E, 0x4D7F, { 0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9 } };

#ifndef INVALID_PROCESSTRACE_HANDLE
#define INVALID_PROCESSTRACE_HANDLE ((TRACEHANDLE)INVALID_HANDLE_VALUE)
#endif

static bool        g_gEtwActive   = false;
static TRACEHANDLE g_gEtwSession  = 0;
static TRACEHANDLE g_gEtwOpen     = INVALID_PROCESSTRACE_HANDLE;
static HANDLE      g_gEtwThread   = NULL;
static SRWLOCK     g_gEtwLock     = SRWLOCK_INIT;
static std::unordered_map<DWORD, ULONG> g_gEtwCounts;
static ULONGLONG   g_gEtwLastSnap = 0;

// EVENT_TRACE_PROPERTIES demands trailing space for the logger name.
struct GuardEtwProps { EVENT_TRACE_PROPERTIES p; wchar_t name[64]; };

static void GuardEtwInitProps(GuardEtwProps& props)
{
    ZeroMemory(&props, sizeof(props));
    props.p.Wnode.BufferSize    = sizeof(props);
    props.p.Wnode.ClientContext = 1;
    props.p.Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    props.p.BufferSize          = 64;
    props.p.MinimumBuffers      = 4;
    props.p.MaximumBuffers      = 8;
    props.p.FlushTimer          = 1;
    props.p.LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    props.p.LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
}

static VOID WINAPI GuardPresentEtwCallback(PEVENT_RECORD rec)
{
    USHORT id = rec->EventHeader.EventDescriptor.Id;
    // DXGI: 42 = Present_Start, 55 = PresentMultiplaneOverlay_Start. D3D9: 1 = Present_Start.
    bool present =
        (IsEqualGUID(rec->EventHeader.ProviderId, kGuardEtwDxgi) && (id == 42 || id == 55)) ||
        (IsEqualGUID(rec->EventHeader.ProviderId, kGuardEtwD3D9) && id == 1);
    if (!present) return;
    AcquireSRWLockExclusive(&g_gEtwLock);
    g_gEtwCounts[rec->EventHeader.ProcessId]++;
    ReleaseSRWLockExclusive(&g_gEtwLock);
}

static DWORD WINAPI GuardPresentEtwThread(LPVOID)
{
    ProcessTrace(&g_gEtwOpen, 1, NULL, NULL);   // blocks until the session stops
    return 0;
}

static bool GuardPresentEtwStart()
{
    GuardEtwProps props;
    GuardEtwInitProps(props);
    ULONG rc = StartTraceW(&g_gEtwSession, kGuardEtwName, &props.p);
    if (rc == ERROR_ALREADY_EXISTS)
    {
        // ETW sessions are kernel objects and outlive a crashed owner - reap
        // the stale one and claim the name afresh.
        GuardEtwInitProps(props);
        ControlTraceW(0, kGuardEtwName, &props.p, EVENT_TRACE_CONTROL_STOP);
        GuardEtwInitProps(props);
        rc = StartTraceW(&g_gEtwSession, kGuardEtwName, &props.p);
    }
    if (rc != ERROR_SUCCESS)
    {
        GLog("present watch: StartTrace failed (%lu)%s", rc,
               rc == ERROR_ACCESS_DENIED ? " - run elevated or join Performance Log Users" : "");
        return false;
    }

    EnableTraceEx2(g_gEtwSession, &kGuardEtwDxgi, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                   TRACE_LEVEL_INFORMATION, 0, 0, 0, NULL);
    EnableTraceEx2(g_gEtwSession, &kGuardEtwD3D9, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                   TRACE_LEVEL_INFORMATION, 0, 0, 0, NULL);

    EVENT_TRACE_LOGFILEW lf = {};
    lf.LoggerName          = (LPWSTR)kGuardEtwName;
    lf.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = GuardPresentEtwCallback;
    g_gEtwOpen = OpenTraceW(&lf);
    if (g_gEtwOpen == INVALID_PROCESSTRACE_HANDLE)
    {
        GLog("present watch: OpenTrace failed (%lu)", GetLastError());
        GuardEtwInitProps(props);
        ControlTraceW(g_gEtwSession, NULL, &props.p, EVENT_TRACE_CONTROL_STOP);
        return false;
    }
    g_gEtwThread = CreateThread(NULL, 0, GuardPresentEtwThread, NULL, 0, NULL);
    if (!g_gEtwThread)
    {
        CloseTrace(g_gEtwOpen);
        g_gEtwOpen = INVALID_PROCESSTRACE_HANDLE;
        GuardEtwInitProps(props);
        ControlTraceW(g_gEtwSession, NULL, &props.p, EVENT_TRACE_CONTROL_STOP);
        return false;
    }
    g_gEtwLastSnap = GetTickCount64();
    g_gEtwActive   = true;
    GLog("present watch armed (threshold %.0f presents/sec)", g_gCfg.gamePresentFps);
    return true;
}

static void GuardPresentEtwStop()
{
    if (!g_gEtwActive) return;
    GuardEtwProps props;
    GuardEtwInitProps(props);
    ControlTraceW(g_gEtwSession, NULL, &props.p, EVENT_TRACE_CONTROL_STOP);
    if (g_gEtwOpen != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(g_gEtwOpen);
        g_gEtwOpen = INVALID_PROCESSTRACE_HANDLE;
    }
    if (g_gEtwThread)
    {
        WaitForSingleObject(g_gEtwThread, 2000);
        CloseHandle(g_gEtwThread);
        g_gEtwThread = NULL;
    }
    g_gEtwActive = false;
}

// Presents/sec for one PID since the previous call. Clears the whole tally so
// each scan reads a fresh window; must therefore be called exactly once per scan.
static double GuardPresentSample(DWORD pid, ULONGLONG now)
{
    ULONG count = 0;
    AcquireSRWLockExclusive(&g_gEtwLock);
    if (pid)
    {
        auto it = g_gEtwCounts.find(pid);
        if (it != g_gEtwCounts.end()) count = it->second;
    }
    g_gEtwCounts.clear();
    ReleaseSRWLockExclusive(&g_gEtwLock);
    double sec = (now - g_gEtwLastSnap) / 1000.0;
    g_gEtwLastSnap = now;
    return sec > 0.05 ? count / sec : 0.0;
}

// A high present rate alone is not a game - browsers and video players push
// 24-60 fps too. The species test: the process either lives in a game store's
// install grounds, or has BOTH a renderer and a game-input library loaded (the
// conjunction matters; Chromium loads Direct3D, and will even load XInput when
// a gamepad is plugged in, but games rarely present fast without both).
// Positive verdicts are cached by PID; negatives re-examined each scan since
// modules can load late.
static bool GuardGameProcess(DWORD pid)
{
    static std::vector<DWORD> knownGames;
    for (DWORD p : knownGames) if (p == pid) return true;

    wchar_t path[MAX_PATH] = {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h)
    {
        DWORD n = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(h, 0, path, &n)) CharLowerW(path);
        else path[0] = 0;
        CloseHandle(h);
    }

    bool game = false;
    static const wchar_t* kStoreDirs[] = {
        L"\\steamapps\\common\\", L"\\epic games\\", L"\\gog galaxy\\games\\",
        L"\\gog games\\", L"\\xboxgames\\", L"\\riot games\\" };
    if (path[0])
        for (auto* d : kStoreDirs)
            if (wcsstr(path, d)) { game = true; break; }

    if (!game)
    {
        bool renderer = false, input = false;
        static const wchar_t* kRender[] = { L"d3d9.dll", L"d3d11.dll", L"d3d12.dll",
                                            L"opengl32.dll", L"vulkan-1.dll" };
        static const wchar_t* kInput[]  = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll",
                                            L"dinput8.dll", L"gameinput.dll" };
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W me = {}; me.dwSize = sizeof(me);
            if (Module32FirstW(snap, &me))
            {
                do
                {
                    if (!renderer)
                        for (auto* r : kRender) if (!_wcsicmp(me.szModule, r)) { renderer = true; break; }
                    if (!input)
                        for (auto* i : kInput)  if (!_wcsicmp(me.szModule, i)) { input = true; break; }
                }
                while ((!renderer || !input) && Module32NextW(snap, &me));
            }
            CloseHandle(snap);
        }
        game = renderer && input;
    }

    if (game)
    {
        if (knownGames.size() > 32) knownGames.clear();   // PIDs recycle; keep the cache short-lived
        knownGames.push_back(pid);
    }
    return game;
}

// True when the foreground window is a fullscreen or borderless application:
// it blankets its whole monitor and carries no caption. The caption test is what
// separates borderless from an ordinary maximized window - a maximized captioned
// window also overhangs its monitor (by its resize borders), most visibly with an
// auto-hidden taskbar, and must not pause the guard. Shell surfaces that
// legitimately cover a monitor (desktop, task view, alt-tab host) are excluded
// by class, as are cloaked UWP remnants and our own windows.
static bool GuardFullscreenForeground()
{
    HWND w = GetForegroundWindow();
    if (!w || w == GetShellWindow()) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return false;

    if (!IsWindowVisible(w) || IsIconic(w)) return false;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return false;

    wchar_t cls[64] = {};
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    static const wchar_t* kShellClasses[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"MultitaskingViewFrame", L"XamlExplorerHostIslandWindow",
        L"ForegroundStaging", L"TaskListThumbnailWnd", L"Windows.UI.Core.CoreWindow" };
    for (auto* c : kShellClasses)
        if (!_wcsicmp(cls, c)) return false;

    LONG style = GetWindowLongW(w, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;

    RECT wr;
    if (!GetWindowRect(w, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return false;
    return wr.left  <= mi.rcMonitor.left  && wr.top    <= mi.rcMonitor.top
        && wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
}

// Drive dwm.exe to the desired injection state. Returns 1 = injected, 0 = not
// injected, -1 = failure (dwm not found, impersonation / inject / unload failed).
// Impersonates SYSTEM per call - dwm.exe is owned by the DWM-N virtual account, so
// a full-rights OpenProcess + CreateRemoteThread needs the SYSTEM token. Unload
// here deliberately does NOT delete the installed DLL (unlike --unload); we mean
// to re-inject it when the game leaves.
static int GuardApplyInjection(bool wantInjected, const char* installedDll)
{
    if (!ImpersonateSystem()) { GLog("guard: ImpersonateSystem failed"); return -1; }
    int result = -1;
    DWORD pid = FindDwmInCurrentSession();
    if (!pid)
    {
        GLog("guard: dwm.exe not found in session");
    }
    else
    {
        HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
                                | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_CREATE_THREAD,
                                  FALSE, pid);
        if (!proc)
        {
            GLog("guard: OpenProcess(dwm.exe pid %lu) failed (%lu)", pid, GetLastError());
        }
        else
        {
            HMODULE existing = FindLoadedModule(proc, "dvhdr.dll");
            if (wantInjected)
            {
                if (existing) result = 1;
                else if (EnsurePayloadInstalled(NULL) && Inject(proc, installedDll)) result = 1;
            }
            else
            {
                if (!existing) result = 0;
                else if (Unload(proc, existing)) result = 0;
            }
            CloseHandle(proc);
        }
    }
    RevertToSelf();
    return result;
}

// True if a game currently holds the foreground: geometric fullscreen/borderless,
// OR a windowed process presenting at game cadence AND shaped like a game. The ETW
// present tally is sampled (and cleared) here, so this MUST be called exactly once
// per scan. outFps / outVia are filled for logging.
static bool GuardDetectGame(ULONGLONG now, double* outFps, bool* outVia)
{
    *outFps = 0.0; *outVia = false;
    HWND  fg  = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);

    bool hit = g_gCfg.pauseOnFullscreen && GuardFullscreenForeground();

    if (g_gEtwActive)
    {
        double fps = GuardPresentSample(pid, now);   // keep the tally window one scan wide
        *outFps = fps;
        if (!hit && pid && fps >= g_gCfg.gamePresentFps && GuardGameProcess(pid))
        {
            hit = true;
            *outVia = true;
        }
    }
    return hit;
}

static void GuardLogForeground(const char* verb, double fps, bool via, int rc)
{
    wchar_t title[128] = {};
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowTextW(fg, title, ARRAYSIZE(title));
    if (via)
        GLog("game presenting at %.0f fps (\"%ls\") - shader %s", fps, title, verb);
    else
        GLog("fullscreen game (\"%ls\") - shader %s", title, verb);
}

// ---- guard tray icon --------------------------------------------------------
// A single tray icon represents the running guard. Its lower-right status dot is
// green while the shader is injected (dwm.exe is tonemapping) and red while it is
// unloaded - either paused for a foreground game or manually disabled. Right-click
// offers the current state, a manual enable / disable toggle and an Exit item; a
// left-click flips the toggle. Modelled on OLEDSaver's tray watcher.

#define GUARD_TRAYMSG   (WM_APP + 1)
#define GUARD_ID_TOGGLE 1001
#define GUARD_ID_EXIT   1002

static const wchar_t*  kGuardWndClass      = L"DVHDR_DWM_GuardTray";
static HWND            g_gTrayWnd          = NULL;
static NOTIFYICONDATAW g_gNid              = {};
static UINT            g_gWmTaskbarCreated = 0;
static bool            g_gTonemapping      = false;  // drives the dot: true = green, false = red
static bool            g_gQuit             = false;  // set by the tray Exit item / session end
static bool            g_gExitUnload       = false;  // clean shutdown: unload the shader + restore MPO on exit
static bool            g_gManualDisabled   = false;  // manual override: keep the shader unloaded regardless of games
static bool            g_gToggleReq        = false;  // tray requested a manual enable / disable flip

static const wchar_t* GuardTipText()
{
    if (g_gManualDisabled) return L"DVHDR-DWM: disabled (manual)";
    return g_gTonemapping ? L"DVHDR-DWM: tonemapping (shader active)"
                          : L"DVHDR-DWM: paused - game foreground";
}

// Compose the tray icon: the app icon overlaid with a status dot in the
// lower-right - green while tonemapping, red while paused for a game. Drawn
// straight into a premultiplied-alpha DIB so the dot keeps a clean edge over the
// icon art. The caller owns the returned HICON and must DestroyIcon it.
static HICON GuardMakeStatusIcon(bool tonemapping)
{
    const int cx = GetSystemMetrics(SM_CXSMICON);
    const int cy = GetSystemMetrics(SM_CYSMICON);
    HINSTANCE hinst = GetModuleHandleW(NULL);

    bool ownBase = true;
    HICON base = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, cx, cy, 0);
    if (!base) { base = LoadIconW(NULL, IDI_APPLICATION); ownBase = false; }

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = cx;
    bi.bmiHeader.biHeight      = -cy;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD* px = NULL;
    HBITMAP color  = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, NULL, 0);
    HBITMAP oldbmp = (HBITMAP)SelectObject(dc, color);

    if (base && px) DrawIconEx(dc, 0, 0, base, cx, cy, 0, NULL, DI_NORMAL);

    if (px)
    {
        const double radius = (cx * 0.30 < 3.0) ? 3.0 : cx * 0.30;
        const double border = (cx / 14.0 < 1.0) ? 1.0 : cx / 14.0;
        const double ccx = cx - radius - 1.0;   // centre near the lower-right corner
        const double ccy = cy - radius - 1.0;
        const int fR = tonemapping ? 46  : 226;   // fill: green vs red
        const int fG = tonemapping ? 204 : 60;
        const int fB = tonemapping ? 87  : 60;
        const int oR = 20, oG = 20, oB = 20;      // dark outline ring for contrast

        for (int y = 0; y < cy; y++)
        {
            for (int x = 0; x < cx; x++)
            {
                double dx = x - ccx, dy = y - ccy;
                double d  = sqrt(dx * dx + dy * dy);
                double a  = radius + 0.5 - d;                 // overall dot coverage
                if (a <= 0.0) continue;
                if (a > 1.0) a = 1.0;
                double f = radius - border + 0.5 - d;         // fill (1) vs outline (0)
                if (f < 0.0) f = 0.0; else if (f > 1.0) f = 1.0;

                int sr = (int)(oR * (1.0 - f) + fR * f);
                int sg = (int)(oG * (1.0 - f) + fG * f);
                int sb = (int)(oB * (1.0 - f) + fB * f);

                DWORD dst = px[y * cx + x];
                int db = dst & 0xFF, dg = (dst >> 8) & 0xFF, dr = (dst >> 16) & 0xFF, da = (dst >> 24) & 0xFF;

                // premultiplied "over": the dot is opaque, so coverage a is its alpha.
                int nb = (int)(sb * a + db * (1.0 - a));
                int ng = (int)(sg * a + dg * (1.0 - a));
                int nr = (int)(sr * a + dr * (1.0 - a));
                int na = (int)(255.0 * a + da * (1.0 - a));
                px[y * cx + x] = (DWORD)((na << 24) | (nr << 16) | (ng << 8) | nb);
            }
        }
    }

    SelectObject(dc, oldbmp);
    DeleteDC(dc);

    // Monochrome AND mask, zeroed so the colour bitmap's alpha governs the shape.
    int stride = ((cx + 15) / 16) * 2;
    std::vector<BYTE> maskBits((size_t)stride * cy, 0);
    HBITMAP mask = CreateBitmap(cx, cy, 1, 1, maskBits.data());

    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    if (ownBase && base) DestroyIcon(base);
    return icon;
}

// Register the icon with the shell. A shell busy with a logon storm can time the
// add out AFTER actually planting the icon, so ERROR_TIMEOUT probes with
// NIM_MODIFY instead of assuming failure, per the Shell_NotifyIcon docs.
static bool GuardTrayAdd()
{
    g_gNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    if (Shell_NotifyIconW(NIM_ADD, &g_gNid)) return true;
    return GetLastError() == ERROR_TIMEOUT && Shell_NotifyIconW(NIM_MODIFY, &g_gNid);
}

static LRESULT CALLBACK GuardWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    // Explorer broadcasts TaskbarCreated when it (re)builds the taskbar; re-add
    // the icon, which may have been lost to an Explorer restart or a start before
    // the shell existed.
    if (g_gWmTaskbarCreated && msg == g_gWmTaskbarCreated && h == g_gTrayWnd)
    {
        GuardTrayAdd();
        return 0;
    }

    switch (msg)
    {
    case GUARD_TRAYMSG:
        if (h == g_gTrayWnd)
        {
            switch (LOWORD(lp))
            {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                g_gToggleReq = true;    // left-click flips injection on / off
                break;
            case WM_CONTEXTMENU:
            case WM_RBUTTONUP:
            {
                POINT pt; GetCursorPos(&pt);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                            g_gManualDisabled ? L"Injection: disabled (manual)"
                          : g_gTonemapping    ? L"Tonemapping: active"
                                              : L"Tonemapping: paused (game)");
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, MF_STRING | (g_gManualDisabled ? 0u : MF_CHECKED),
                            GUARD_ID_TOGGLE, L"Injection enabled");
                AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
                AppendMenuW(menu, MF_STRING, GUARD_ID_EXIT, L"Exit guard");
                SetForegroundWindow(h);    // so the menu dismisses on click-away
                int cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                              pt.x, pt.y, 0, h, NULL);
                DestroyMenu(menu);
                if      (cmd == GUARD_ID_TOGGLE) g_gToggleReq = true;
                else if (cmd == GUARD_ID_EXIT)   { g_gQuit = true; g_gExitUnload = true; PostQuitMessage(0); }
                break;
            }
            }
        }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        g_gQuit = true;
        return 0;
    case WM_DESTROY:
        if (h == g_gTrayWnd) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void GuardCreateTrayWindow()
{
    HINSTANCE hinst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = GuardWndProc;
    wc.hInstance     = hinst;
    wc.lpszClassName = kGuardWndClass;
    RegisterClassExW(&wc);

    // Hidden top-level control window that owns the tray icon and receives the
    // TaskbarCreated broadcast.
    g_gTrayWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kGuardWndClass, L"DVHDR-DWM Guard",
                                 WS_POPUP, 0, 0, 0, 0, NULL, NULL, hinst, NULL);

    g_gWmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // The guard runs elevated (it injects into dwm.exe), so UIPI would drop
    // Explorer's medium-integrity TaskbarCreated broadcast; let it through so the
    // icon re-appears after an Explorer restart.
    if (g_gTrayWnd)
        ChangeWindowMessageFilterEx(g_gTrayWnd, g_gWmTaskbarCreated, MSGFLT_ALLOW, NULL);
}

static void GuardTrayInit()
{
    if (!g_gTrayWnd) return;
    g_gNid = {};
    g_gNid.cbSize           = sizeof(g_gNid);
    g_gNid.hWnd             = g_gTrayWnd;
    g_gNid.uID              = 1;
    g_gNid.uCallbackMessage = GUARD_TRAYMSG;
    g_gNid.hIcon            = GuardMakeStatusIcon(g_gTonemapping);
    if (!g_gNid.hIcon) g_gNid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(g_gNid.szTip, GuardTipText(), ARRAYSIZE(g_gNid.szTip));
    GuardTrayAdd();
}

// Repaint the dot and refresh the tooltip after an injection-state flip. The
// NIM_MODIFY failing means the icon is gone (Explorer died without a
// TaskbarCreated we saw), so reinstate it.
static void GuardTrayRefresh()
{
    if (!g_gTrayWnd) return;
    HICON fresh = GuardMakeStatusIcon(g_gTonemapping);
    HICON old   = g_gNid.hIcon;
    if (fresh) g_gNid.hIcon = fresh;
    lstrcpynW(g_gNid.szTip, GuardTipText(), ARRAYSIZE(g_gNid.szTip));
    g_gNid.uFlags = NIF_ICON | NIF_TIP;
    if (!Shell_NotifyIconW(NIM_MODIFY, &g_gNid)) GuardTrayAdd();
    if (fresh && old && old != fresh) DestroyIcon(old);
}

// Cheap periodic re-assert: if the shell dropped our icon (Explorer restart, or
// our first add landed before the taskbar existed) the NIM_MODIFY fails and we
// re-add it. Reached once per scan, so a lost icon returns within a couple seconds.
static void GuardTrayEnsure()
{
    if (!g_gTrayWnd || !g_gNid.cbSize) return;
    g_gNid.uFlags = NIF_TIP;
    if (!Shell_NotifyIconW(NIM_MODIFY, &g_gNid)) GuardTrayAdd();
}

static void GuardTrayDestroy()
{
    if (g_gNid.cbSize) Shell_NotifyIconW(NIM_DELETE, &g_gNid);
    if (g_gNid.hIcon)  { DestroyIcon(g_gNid.hIcon); g_gNid.hIcon = NULL; }
    if (g_gTrayWnd)    { DestroyWindow(g_gTrayWnd);  g_gTrayWnd = NULL; }
    UnregisterClassW(kGuardWndClass, GetModuleHandleW(NULL));
}

// Set the resident enable state: write the registry flag the shader polls and
// reflect it on the tray dot. The shader stays loaded either way - enabled renders
// with MPO forced off, disabled passes presents through with MPO restored. No
// inject or unload, so no attach/detach race with DWM.
static void GuardSetEnabled(bool enabled)
{
    WriteEnabledToRegistry(enabled);
    g_gTonemapping = enabled;
    GuardTrayRefresh();
}

static int RunGuard()
{
    HANDLE mutex = CreateMutexW(NULL, FALSE, kGuardMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        msg("guard already running");
        CloseHandle(mutex);
        return 0;
    }
    HANDLE stopEvt = CreateEventW(NULL, TRUE, FALSE, kGuardStopName);

    LoadGuardCfg(g_gCfg);
    GOpenLog();

    // Show the tray icon immediately - before the elevation check or the seed
    // inject - so it always appears the instant the guard starts and never waits
    // on a slow or crashing inject. The status dot is corrected after the seed.
    GuardCreateTrayWindow();
    GuardTrayInit();

    int rc = 0;
    if (!EnableDebugPrivilege())
    {
        msg("Could not enable SeDebugPrivilege - the guard must run elevated");
        GLog("guard: EnableDebugPrivilege failed - run elevated");
        rc = 5;
    }

    char installedDll[MAX_PATH];
    if (rc == 0 && !GetInstalledDllPath(installedDll, sizeof(installedDll)))
    {
        msg("Could not resolve %%SYSTEMROOT%%\\Temp\\dvhdr.dll");
        rc = 3;
    }

    if (rc == 0)
    {
        if (g_gCfg.pauseOnGamePresent) GuardPresentEtwStart();

        // Seed: judge the current foreground directly (no debounce), so a game
        // already up keeps dwm clean and an idle desktop is injected at once.
        ULONGLONG now = GetTickCount64();
        double fps; bool via;
        bool paused = GuardDetectGame(now, &fps, &via);

        // Load the shader once and keep it resident; pausing for games and the tray
        // toggle flip a registry flag it polls, never an unload/re-inject.
        int inj = GuardApplyInjection(true, installedDll);
        GLog("guard armed (fps threshold %.0f) - initial: %s, dll %s",
             g_gCfg.gamePresentFps,
             paused ? "PAUSED (game present)" : "active",
             inj == 1 ? "loaded" : inj == 0 ? "not loaded" : "state unknown");
        GuardSetEnabled(!paused);   // render unless a game already holds the foreground

        int agree = 0;                     // consecutive scans disagreeing with `paused`
        ULONGLONG lastReinject = now;
        ULONGLONG nextScan     = now + kGuardScanMs;

        // A message-pumping wait so the tray icon stays live: wake for the stop
        // event, for tray / window messages, or when the next detection scan is due.
        while (!g_gQuit)
        {
            ULONGLONG t = GetTickCount64();
            DWORD waitMs = (nextScan > t) ? (DWORD)(nextScan - t) : 0;
            DWORD w = MsgWaitForMultipleObjects(1, &stopEvt, FALSE, waitMs, QS_ALLINPUT);
            if (w == WAIT_OBJECT_0) { g_gExitUnload = true; break; }   // --guard-stop: clean shutdown
            if (w == WAIT_OBJECT_0 + 1)                // tray / window messages pending
            {
                MSG m;
                while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
                {
                    if (m.message == WM_QUIT) g_gQuit = true;
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
                if (g_gQuit) break;
            }

            now = GetTickCount64();

            // Apply a manual enable / disable requested from the tray at once,
            // off the scan cadence. Manual disable is an override that keeps the
            // shader unloaded no matter what detection reports; re-enabling hands
            // control back to the automatic game watch (which injects now unless a
            // game currently holds the foreground).
            if (g_gToggleReq)
            {
                g_gToggleReq = false;
                g_gManualDisabled = !g_gManualDisabled;
                GuardSetEnabled(!paused && !g_gManualDisabled);
                GLog("manual toggle: shader %s (resident)", g_gManualDisabled ? "DISABLED" : "ENABLED");
            }

            if (now < nextScan) continue;              // woke for messages; scan not yet due
            nextScan = now + kGuardScanMs;

            GuardTrayEnsure();   // self-heal the tray icon if the shell dropped it

            bool game = GuardDetectGame(now, &fps, &via);
            if (game != paused)
            {
                if (++agree >= 2)          // debounce: ~4s of agreement before flipping
                {
                    agree = 0;
                    paused = game;
                    bool enabled = !paused && !g_gManualDisabled;
                    GuardSetEnabled(enabled);   // flip the flag; the shader stays loaded
                    if (g_gManualDisabled)
                        GLog("game %s (manual override holds - shader stays disabled)", game ? "appeared" : "gone");
                    else if (paused)
                        GuardLogForeground("PAUSED (passthrough, MPO restored)", fps, via, 0);
                    else
                        GLog("game gone - shader ENABLED (rendering)");
                }
            }
            else agree = 0;

            // Keep the shader resident: recover from a restarted dwm.exe by
            // re-injecting when the DLL is gone. Runs regardless of enable state - a
            // disabled shader must still be loaded so it can be re-enabled - and a
            // no-op when already loaded. The flag is re-asserted so a fresh attach
            // reads the current enable state.
            if ((now - lastReinject) >= kGuardReinjectMs)
            {
                lastReinject = now;
                WriteEnabledToRegistry(!paused && !g_gManualDisabled);
                if (GuardApplyInjection(true, installedDll) != 1)
                    GLog("guard: re-inject check could not confirm the DLL is loaded");
            }
        }

        GLog("guard stopping");

        // An explicit stop (tray "Exit guard" or --guard-stop) is a full shutdown:
        // unload the shader so dwm.exe returns to its clean launch state with MPO /
        // DirectFlip re-enabled, and sweep the installed payload away. A session-end
        // exit skips this - dwm.exe is being torn down anyway. Leaving the shader
        // injected with no watcher is exactly the unmanaged state that breaks a
        // game's G-Sync, so a deliberate stop must not leave it behind.
        if (g_gExitUnload)
        {
            int r = GuardApplyInjection(false, installedDll);
            GLog("exit: shader %s", r == 0 ? "UNLOADED, MPO restored" : "unload could not be confirmed");
            RemovePayloadInstalled();
            WriteEnabledToRegistry(true);   // leave the flag enabled for a future inject
        }

        GuardPresentEtwStop();
    }

    GuardTrayDestroy();   // common: remove the icon whether or not we entered the loop
    if (g_gLog) { fclose(g_gLog); g_gLog = NULL; }
    if (stopEvt) CloseHandle(stopEvt);
    if (mutex)   CloseHandle(mutex);
    return rc;
}

static int RunGuardStop()
{
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, kGuardStopName);
    if (!e) { msg("no running guard found"); return 0; }
    SetEvent(e);
    CloseHandle(e);
    msg("guard stop signalled");
    return 0;
}

// Signal a running --guard watcher to stop. Returns true if one was found. The
// guard's stop path already unloads the shader, removes the payload and deletes
// its tray icon, so an explicit --unload hands off to it rather than fighting it
// (a bare unload would leave the guard's tray icon behind, and it would re-inject).
static bool SignalGuardStop()
{
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, kGuardStopName);
    if (!e) return false;
    SetEvent(e);
    CloseHandle(e);
    return true;
}

enum class Mode { Auto, Force, Unload, Status, List, Guard, GuardStop };

int main(int argc, char** argv)
{
    AttachParentConsole();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Mode mode = Mode::Auto;
    std::vector<int> monitorIndices;
    for (int i = 1; i < argc; i++)
    {
        if (!_stricmp(argv[i], "--force"))       mode = Mode::Force;
        else if (!_stricmp(argv[i], "--unload")) mode = Mode::Unload;
        else if (!_stricmp(argv[i], "--status")) mode = Mode::Status;
        else if (!_stricmp(argv[i], "--list"))   mode = Mode::List;
        else if (!_stricmp(argv[i], "--guard"))      mode = Mode::Guard;
        else if (!_stricmp(argv[i], "--guard-stop")) mode = Mode::GuardStop;
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
        else if (!_stricmp(argv[i], "-h") || !_stricmp(argv[i], "--help"))
        {
            OpenOutputGap();
            printf("dvhdrloader [--force|--unload|--status|--list] [-m N[,N...]] [-q]\n");
            printf("            [--guard] [--guard-stop]\n");
            printf("  (none)        inject dvhdr.dll if absent, otherwise no-op\n");
            printf("  --force       unload + reinject (reloads config)\n");
            printf("  --unload      remove dvhdr.dll from dwm.exe\n");
            printf("  --status      report whether dvhdr.dll is currently loaded\n");
            printf("  --list        enumerate displays with index + coords\n");
            printf("  -m N[,N...]   tonemap these display number(s) (per-screen caps via [Display.N] in the ini), then force-reinject\n");
            printf("  --guard       persistent watcher: unload the shader while a game is foreground (restores MPO/G-Sync), re-inject after. Elevated. See [Guard]\n");
            printf("  --guard-stop  stop a running --guard watcher and unload the shader (restores MPO); same as the tray Exit item\n");
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

    // The game guard is a self-contained, persistent watcher; it owns the
    // inject/unload cycle itself and returns here without falling through to the
    // one-shot injection path below.
    if (mode == Mode::GuardStop) return RunGuardStop();
    if (mode == Mode::Guard)     return RunGuard();

    // --unload while a guard is running: the guard owns the inject/unload cycle,
    // so hand off to it. It unloads the shader, removes the payload and closes its
    // tray icon on exit. Doing our own unload here would leave that icon behind
    // and the guard would just re-inject on its next tick.
    if (mode == Mode::Unload && SignalGuardStop())
    {
        msg("guard was running - signalled it to stop; it unloads the shader and removes its tray icon");
        return 0;
    }

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
        WriteEnabledToRegistry(true);   // a manual inject always renders, regardless of a prior guard-disabled flag
        if (existing && !Unload(proc, existing)) { msg("force: unload failed"); rc = 6; break; }
        if (!EnsurePayloadInstalled(NULL)) { rc = 10; break; }
        if (Inject(proc, installedDll)) msg("injected");
        else { msg("inject failed"); rc = 7; }
        break;

    case Mode::Auto:
        WriteEnabledToRegistry(true);
        if (existing) { msg("already loaded"); break; }
        if (!EnsurePayloadInstalled(NULL)) { rc = 10; break; }
        if (Inject(proc, installedDll)) msg("injected");
        else { msg("inject failed"); rc = 7; }
        break;

    case Mode::List:
    case Mode::Guard:
    case Mode::GuardStop:
        // Handled earlier (each returns before this switch); here only to silence -Wswitch.
        break;
    }

    RevertToSelf();

    CloseHandle(proc);
    return rc;
}
