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
// REG_MULTI_SZ list of "left,top" strings. The DLL reads it on attach. No
// side files beside the loader.
//
// Flags:
//   (none)            inject if absent, exit silently if already present
//   --force           unload + reinject (forces config reload)
//   --unload          remove the DLL from dwm.exe
//   --status          report whether dvhdr.dll is currently loaded
//   --list            enumerate displays with index + coords, exit
//   -m N[,N,...]      write monitor coords to registry for the given display
//                     number(s), then force-reinject. Mirrors ApplyIccLut -m.
//   -q/--silent       suppress console output

#include "pch.h"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

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

// HKLM\SOFTWARE\DVHDR-DWM\Monitors = REG_MULTI_SZ list of "left,top" strings.
static bool WriteMonitorsToRegistry(const std::vector<DisplayInfo>& selected)
{
    std::vector<wchar_t> buf;
    for (auto& d : selected)
    {
        wchar_t line[64];
        swprintf_s(line, L"%d,%d", d.left, d.top);
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

enum class Mode { Auto, Force, Unload, Status, List };

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
            printf("  (none)        inject dvhdr.dll if absent, otherwise no-op\n");
            printf("  --force       unload + reinject (reloads config)\n");
            printf("  --unload      remove dvhdr.dll from dwm.exe\n");
            printf("  --status      report whether dvhdr.dll is currently loaded\n");
            printf("  --list        enumerate displays with index + coords\n");
            printf("  -m N[,N...]   set HKLM\\SOFTWARE\\DVHDR-DWM\\Monitors for these display numbers, then force-reinject\n");
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
        // Handled earlier; here only to silence -Wswitch.
        break;
    }

    RevertToSelf();

    CloseHandle(proc);
    return rc;
}
