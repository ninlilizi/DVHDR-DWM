// dllmain.cpp — proxy dxgi.dll entry point. On attach it resolves the genuine
// system dxgi.dll for the export forwarders, reads dvhdr.ini from beside this
// DLL, and kicks the background thread that installs the Present hook.

#include "framework.h"
#include "config.h"

bool Exports_Init();
void Hook_Start();
void Hook_Stop();
void Effect11_Shutdown();
void Effect12_Shutdown();

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        if (!Exports_Init()) return FALSE;   // genuine dxgi missing — fatal
        Config_SetSelfModule(hModule);
        Config_Load();
        Hook_Start();
        break;

    case DLL_PROCESS_DETACH:
        Hook_Stop();
        Effect11_Shutdown();
        Effect12_Shutdown();
        break;
    }
    return TRUE;
}
