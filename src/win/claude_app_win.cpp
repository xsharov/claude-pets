#include "claude_app.h"

#include "log.h"
#include "util.h"
#include "win/win_util.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <mutex>
#include <unordered_map>

namespace cp {

namespace {

bool IsClaudeDesktopPath(const std::wstring& imagePath) {
    if (imagePath.empty()) {
        return false;
    }
    const std::wstring lower = ToLower(imagePath);
    if (FileNameOf(std::wstring_view(lower)) != L"claude.exe") {
        return false;
    }
    if (lower.find(L"\\claude-code\\") != std::wstring::npos || lower.find(L"\\.local\\") != std::wstring::npos) {
        return false;
    }
    return lower.find(L"\\windowsapps\\claude_") != std::wstring::npos || lower.find(L"\\anthropicclaude\\") != std::wstring::npos ||
           lower.find(L"\\programs\\claude\\") != std::wstring::npos || lower.find(L"\\claude\\app\\") != std::wstring::npos;
}

std::wstring ProcessImagePath(DWORD processId) {
    static std::mutex cacheLock;
    static std::unordered_map<DWORD, std::pair<int64_t, std::wstring>> cache;
    const int64_t now = UnixNowMs();
    {
        std::lock_guard<std::mutex> lock(cacheLock);
        auto found = cache.find(processId);
        if (found != cache.end() && now - found->second.first < 15000) {
            return found->second.second;
        }
    }
    std::wstring path;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process != nullptr) {
        wchar_t buffer[2048] = {};
        DWORD size = static_cast<DWORD>(std::size(buffer));
        if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
            path.assign(buffer, size);
        }
        CloseHandle(process);
    }
    std::lock_guard<std::mutex> lock(cacheLock);
    if (cache.size() > 512) {
        cache.clear();
    }
    cache[processId] = {now, path};
    return path;
}

int64_t ProcessStartedMs(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return 0;
    }
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    const bool known = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    CloseHandle(process);
    return known ? FileTimeToUnixMs(created) : 0;
}

bool IsClaudeDesktopWindow(HWND window) {
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId != 0 && IsClaudeDesktopPath(ProcessImagePath(processId));
}

struct WindowSearch {
    HWND best = nullptr;
    long long bestScore = -1;
};

BOOL CALLBACK CollectClaudeWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    if (GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    wchar_t className[64] = {};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (wcscmp(className, L"Chrome_WidgetWin_1") != 0) {
        return TRUE;
    }
    if (GetWindowTextLengthW(window) == 0) {
        return TRUE;
    }
    if (!IsClaudeDesktopWindow(window)) {
        return TRUE;
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }
    RECT rect = {};
    GetWindowRect(window, &rect);
    long long score = static_cast<long long>(rect.right - rect.left) * (rect.bottom - rect.top);
    if (IsWindowVisible(window) || IsIconic(window)) {
        score += 1LL << 40;
    }
    if (score > search->bestScore) {
        search->bestScore = score;
        search->best = window;
    }
    return TRUE;
}

std::wstring ClaudePackageFamily() {
    const std::wstring packages = JoinPath(KnownFolderPath(FOLDERID_LocalAppData), L"Packages");
    for (const std::wstring& package : ListDirectories(packages)) {
        if (StartsWith(ToLower(package), L"claude_")) {
            return package;
        }
    }
    return {};
}

bool LaunchClaude() {
    const std::wstring family = ClaudePackageFamily();
    if (!family.empty()) {
        Microsoft::WRL::ComPtr<IApplicationActivationManager> manager;
        if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager)))) {
            DWORD processId = 0;
            const std::wstring appId = family + L"!Claude";
            if (SUCCEEDED(manager->ActivateApplication(appId.c_str(), nullptr, AO_NONE, &processId))) {
                return true;
            }
        }
    }
    const std::wstring squirrel = JoinPath(JoinPath(KnownFolderPath(FOLDERID_LocalAppData), L"AnthropicClaude"), L"claude.exe");
    if (FileExists(squirrel)) {
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", squirrel.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32;
    }
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"claude://code/continue?session=last", nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool ForceForeground(HWND window) {
    if (SetForegroundWindow(window)) {
        return true;
    }
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread = foreground != nullptr ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD currentThread = GetCurrentThreadId();
    bool attached = false;
    if (foregroundThread != 0 && foregroundThread != currentThread) {
        attached = AttachThreadInput(currentThread, foregroundThread, TRUE) != FALSE;
    }
    BringWindowToTop(window);
    const bool result = SetForegroundWindow(window) != FALSE;
    if (attached) {
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    }
    LogLine("force foreground attached=" + std::to_string(attached) + " result=" + std::to_string(result));
    return result;
}

HWND FindClaudeMainWindow() {
    WindowSearch search;
    EnumWindows(CollectClaudeWindow, reinterpret_cast<LPARAM>(&search));
    return search.best;
}

}

std::vector<std::wstring> ClaudeDesktopDataDirectories() {
    std::vector<std::wstring> directories;
    const std::wstring localAppData = KnownFolderPath(FOLDERID_LocalAppData);
    const std::wstring userData = JoinPath(localAppData, L"Claude-Data");
    for (const std::wstring& install : ListDirectories(userData)) {
        directories.push_back(JoinPath(userData, install));
    }
    directories.push_back(JoinPath(KnownFolderPath(FOLDERID_RoamingAppData), L"Claude"));
    const std::wstring packages = JoinPath(localAppData, L"Packages");
    for (const std::wstring& package : ListDirectories(packages)) {
        if (StartsWith(ToLower(package), L"claude_")) {
            directories.push_back(JoinPath(JoinPath(packages, package), L"LocalCache\\Roaming\\Claude"));
        }
    }
    return directories;
}

int64_t ClaudeDesktopStartedMs() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    int64_t earliest = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"claude.exe") != 0 || !IsClaudeDesktopPath(ProcessImagePath(entry.th32ProcessID))) {
                continue;
            }
            const int64_t started = ProcessStartedMs(entry.th32ProcessID);
            if (started > 0 && (earliest == 0 || started < earliest)) {
                earliest = started;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return earliest;
}

bool IsClaudeDesktopRunning() {
    return ClaudeDesktopStartedMs() != 0;
}

bool IsClaudeForeground() {
    const HWND foreground = GetForegroundWindow();
    return foreground != nullptr && IsClaudeDesktopWindow(foreground);
}

bool OpenClaudeSession(const std::string& sessionId) {
    AllowSetForegroundWindow(ASFW_ANY);
    const std::wstring url = L"claude://claude.ai/epitaxy/" + Widen(sessionId);
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

void BringClaudeToFront() {
    const HWND window = FindClaudeMainWindow();
    if (window == nullptr || !IsWindowVisible(window)) {
        return;
    }
    if (IsIconic(window)) {
        ShowWindow(window, SW_RESTORE);
    }
    ForceForeground(window);
}

bool ActivateClaude() {
    AllowSetForegroundWindow(ASFW_ANY);
    const HWND window = FindClaudeMainWindow();
    if (LogEnabled()) {
        LogLine("activate claude window=" + std::to_string(reinterpret_cast<uintptr_t>(window)) + " visible=" + std::to_string(window != nullptr && IsWindowVisible(window)) +
                " iconic=" + std::to_string(window != nullptr && IsIconic(window)));
    }
    if (window != nullptr && (IsWindowVisible(window) || IsIconic(window))) {
        if (IsIconic(window)) {
            ShowWindow(window, SW_RESTORE);
        }
        return ForceForeground(window);
    }
    if (IsClaudeDesktopRunning()) {
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"claude://code/continue?session=last", nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32;
    }
    return LaunchClaude();
}

}
