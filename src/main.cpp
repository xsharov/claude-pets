#include "app.h"
#include "diagnostics.h"
#include "hooks.h"
#include "util.h"
#include "version.h"

#include <objbase.h>
#include <windows.h>

#include <cwchar>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const wchar_t* commandLine = GetCommandLineW();
    if (std::wcsstr(commandLine, cp::kHookArgument) != nullptr) {
        return cp::RunHookRelay();
    }
    if (std::wcsstr(commandLine, L"--version") != nullptr) {
        cp::WriteConsoleText(std::string("claude-pets ") + CP_VERSION_TEXT + "\n");
        return 0;
    }
    if (std::wcsstr(commandLine, L"--install-hooks") != nullptr || std::wcsstr(commandLine, L"--uninstall-hooks") != nullptr) {
        const bool install = std::wcsstr(commandLine, L"--install-hooks") != nullptr;
        const bool ok = install ? cp::InstallHooks(cp::ExecutablePath()) : cp::UninstallHooks();
        cp::WriteConsoleText(std::string(ok ? "ok: " : "failed: ") + cp::Narrow(cp::ClaudeSettingsPath()) + "\n");
        return ok ? 0 : 1;
    }
    if (std::wcsstr(commandLine, L"--diagnose") != nullptr) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const int code = cp::RunDiagnostics();
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
        return code;
    }
    const bool startHidden = std::wcsstr(commandLine, L"--hidden") != nullptr;
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\ClaudePets.Instance");
    if (instanceMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        const HWND existing = FindWindowW(L"ClaudePetsOverlay", nullptr);
        if (existing != nullptr) {
            PostMessageW(existing, cp::kShowPetMessage, 0, 0);
        }
        CloseHandle(instanceMutex);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int exitCode = 0;
    {
        cp::App app;
        exitCode = app.Run(instance, startHidden);
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    if (instanceMutex != nullptr) {
        ReleaseMutex(instanceMutex);
        CloseHandle(instanceMutex);
    }
    return exitCode;
}
