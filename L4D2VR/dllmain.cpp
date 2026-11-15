// dllmain.cpp : Defines the entry point for the DLL application.
#include <Windows.h>
#include <iostream>
#include <io.h>
#include <share.h>
#include <string>
#include "game.h"
#include "hooks.h"
#include "vr.h"
#include "sdk.h"

DWORD WINAPI InitL4D2VR(HMODULE hModule)
{
// Release if buggy, so we'll be releasing the debug binary
#ifdef _DEBUG
    AllocConsole();
    FILE *fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
#else
    FILE *fp;
    FILE *log = _fsopen("VR\\portal2vr.log", "w", _SH_DENYWR);
    if (log && freopen_s(&fp, "NUL", "w", stdout) == 0 && _dup2(_fileno(log), _fileno(stdout)) == 0)
        setvbuf(stdout, nullptr, _IONBF, 0);
    if (log)
        fclose(log);
#endif

    // Make sure -insecure is used
    LPWSTR *szArglist;
    int nArgs;
    szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    bool insecureEnabled = false;
    for (int i = 0; i < nArgs; ++i)
    {
        if (_wcsicmp(szArglist[i], L"-insecure") == 0)
            insecureEnabled = true;
    }
    LocalFree(szArglist);

    if (!insecureEnabled)
    {
        wchar_t directory[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, directory);

        std::wstring relaunch = L"cmd.exe /d /c ping -n 3 127.0.0.1 >nul & start \"\" ";
        relaunch += GetCommandLineW();
        relaunch += L" -insecure";

        STARTUPINFOW startup = { sizeof(startup) };
        PROCESS_INFORMATION process = {};
        if (CreateProcessW(nullptr, relaunch.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, directory, &startup, &process))
        {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            ExitProcess(0);
        }

        MessageBoxW(0, L"Portal 2 VR needs the game to run with -insecure and couldn't restart it that way. Add -insecure to Portal 2's launch options in Steam.", L"Portal 2 VR", MB_ICONERROR | MB_OK);
        return 0;
    }

    g_Game = new Game();

    return 0;
}



BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
            CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)InitL4D2VR, hModule, 0, NULL);
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}


