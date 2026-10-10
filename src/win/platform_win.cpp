#include "platform.h"

#include "i18n.h"
#include "overlay_window.h"
#include "resource.h"
#include "util.h"
#include "win/win_util.h"

#include <shellapi.h>

#include <cwchar>
#include <set>
#include <vector>

namespace cp {

namespace {

constexpr UINT kSessionsChangedMessage = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 3;
constexpr int kHotkeyId = 1;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ClaudePets";

std::wstring QuotedExecutable(const std::wstring& executable) {
    return L"\"" + executable + L"\"";
}

std::wstring Trim(std::wstring_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && iswspace(text[begin])) {
        ++begin;
    }
    while (end > begin && iswspace(text[end - 1])) {
        --end;
    }
    return std::wstring(text.substr(begin, end - begin));
}

bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& virtualKey) {
    modifiers = 0;
    virtualKey = 0;
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t plus = text.find(L'+', start);
        const size_t end = plus == std::wstring::npos ? text.size() : plus;
        std::wstring part = ToLower(Trim(std::wstring_view(text).substr(start, end - start)));
        if (!part.empty()) {
            parts.push_back(std::move(part));
        }
        if (plus == std::wstring::npos) {
            break;
        }
        start = plus + 1;
    }
    if (parts.size() < 2) {
        return false;
    }
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::wstring& part = parts[i];
        if (part == L"ctrl" || part == L"control") {
            modifiers |= MOD_CONTROL;
        } else if (part == L"alt") {
            modifiers |= MOD_ALT;
        } else if (part == L"shift") {
            modifiers |= MOD_SHIFT;
        } else if (part == L"win" || part == L"windows") {
            modifiers |= MOD_WIN;
        } else {
            return false;
        }
    }
    const std::wstring& key = parts.back();
    if (key.size() == 1 && ((key[0] >= L'a' && key[0] <= L'z') || (key[0] >= L'0' && key[0] <= L'9'))) {
        virtualKey = static_cast<UINT>(towupper(key[0]));
    } else if (key.size() >= 2 && key[0] == L'f') {
        const int number = _wtoi(key.c_str() + 1);
        if (number >= 1 && number <= 24) {
            virtualKey = VK_F1 + static_cast<UINT>(number - 1);
        }
    } else if (key == L"space") {
        virtualKey = VK_SPACE;
    }
    return modifiers != 0 && virtualKey != 0;
}

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon() { Hide(); }
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    void Show(HWND owner, UINT callbackMessage, HICON icon, const std::wstring& tooltip) {
        data_ = {};
        data_.cbSize = sizeof(data_);
        data_.hWnd = owner;
        data_.uID = 1;
        data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data_.uCallbackMessage = callbackMessage;
        data_.hIcon = icon;
        wcsncpy_s(data_.szTip, tooltip.c_str(), _TRUNCATE);
        if (visible_) {
            Shell_NotifyIconW(NIM_MODIFY, &data_);
            return;
        }
        visible_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    }

    void Hide() {
        if (!visible_) {
            return;
        }
        Shell_NotifyIconW(NIM_DELETE, &data_);
        visible_ = false;
    }

    void Restore() {
        if (visible_) {
            visible_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
        }
    }

private:
    NOTIFYICONDATAW data_ = {};
    bool visible_ = false;
};

HMENU BuildMenu(const std::vector<MenuItem>& items) {
    HMENU menu = CreatePopupMenu();
    for (const MenuItem& item : items) {
        switch (item.kind) {
        case MenuItem::Kind::Separator:
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            break;
        case MenuItem::Kind::Submenu:
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildMenu(item.children)), item.label.c_str());
            break;
        case MenuItem::Kind::Command: {
            std::wstring label = item.label;
            if (!item.shortcut.empty()) {
                label += L"\t" + item.shortcut;
            }
            const UINT flags = MF_STRING | (item.checked ? MF_CHECKED : MF_UNCHECKED) | (item.enabled ? 0 : MF_GRAYED);
            AppendMenuW(menu, flags, item.command, label.c_str());
            break;
        }
        }
    }
    return menu;
}

bool ItemActivates(const std::vector<MenuItem>& items, uint32_t command) {
    for (const MenuItem& item : items) {
        if (item.kind == MenuItem::Kind::Submenu ? ItemActivates(item.children, command) : (item.command == command && item.activates)) {
            return true;
        }
    }
    return false;
}

}

struct Platform::State {
    HWND window = nullptr;
    HICON icon = nullptr;
    UINT taskbarCreatedMessage = 0;
    TrayIcon tray;
    std::set<uint32_t> timers;
    bool quitting = false;
};

Platform::Platform(PlatformEvents& events) : events_(events), state_(std::make_unique<State>()) {}

Platform::~Platform() {
    state_->tray.Hide();
    if (state_->icon != nullptr) {
        DestroyIcon(state_->icon);
        state_->icon = nullptr;
    }
}

bool Platform::Attach(OverlayWindow& window) {
    State& state = *state_;
    state.window = window.Handle();
    state.icon = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    state.taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    window.SetMessageHook([this](UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) {
        State& state = *state_;
        switch (message) {
        case kSessionsChangedMessage:
            events_.OnSessionsChanged();
            result = 0;
            return true;
        case kShowPetMessage:
            events_.OnShowRequested();
            result = 0;
            return true;
        case kTrayMessage:
            switch (LOWORD(lParam)) {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                events_.OnTrayClicked();
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                events_.OnTrayMenu();
                break;
            default:
                break;
            }
            result = 0;
            return true;
        case WM_COPYDATA: {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (data == nullptr || data->dwData != kHookCopyDataId || data->lpData == nullptr || data->cbData > (16u << 20)) {
                return false;
            }
            events_.OnHookEvent(std::string(static_cast<const char*>(data->lpData), data->cbData));
            result = TRUE;
            return true;
        }
        case WM_HOTKEY:
            if (wParam == kHotkeyId) {
                events_.OnHotkey();
                result = 0;
                return true;
            }
            return false;
        case WM_TIMER:
            if (state.timers.count(static_cast<uint32_t>(wParam)) != 0) {
                events_.OnTimer(static_cast<uint32_t>(wParam));
                result = 0;
                return true;
            }
            return false;
        case WM_SETTINGCHANGE:
            if (lParam != 0 && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
                events_.OnThemeChanged();
            }
            return false;
        case WM_QUERYENDSESSION:
            events_.OnSessionEnding();
            result = TRUE;
            return true;
        case WM_ENDSESSION:
            if (wParam != 0) {
                events_.OnSessionEnding();
            }
            return false;
        case WM_CLOSE:
            events_.OnCloseRequested();
            result = 0;
            return true;
        case WM_DESTROY:
            if (!state.quitting) {
                PostQuitMessage(0);
            }
            return false;
        default:
            if (state.taskbarCreatedMessage != 0 && message == state.taskbarCreatedMessage) {
                state.tray.Restore();
                result = 0;
                return true;
            }
            return false;
        }
    });
    return true;
}

void Platform::Run() {
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void Platform::Quit() {
    state_->quitting = true;
    PostQuitMessage(0);
}

void Platform::PostSessionsChanged() {
    PostMessageW(state_->window, kSessionsChangedMessage, 0, 0);
}

void Platform::SetTimer(uint32_t id, uint32_t delayMs) {
    state_->timers.insert(id);
    ::SetTimer(state_->window, id, delayMs, nullptr);
}

void Platform::KillTimer(uint32_t id) {
    ::KillTimer(state_->window, id);
}

bool Platform::RegisterHotkey(const std::wstring& text) {
    UINT modifiers = 0;
    UINT key = 0;
    if (!ParseHotkey(text, modifiers, key)) {
        return false;
    }
    return RegisterHotKey(state_->window, kHotkeyId, modifiers | MOD_NOREPEAT, key) != FALSE;
}

void Platform::UnregisterHotkey() {
    UnregisterHotKey(state_->window, kHotkeyId);
}

void Platform::ShowTray(const std::wstring& tooltip) {
    state_->tray.Show(state_->window, kTrayMessage, state_->icon, tooltip);
}

void Platform::HideTray() {
    state_->tray.Hide();
}

uint32_t Platform::ShowMenu(const std::vector<MenuItem>& items, PointI screen) {
    HMENU menu = BuildMenu(items);
    const HWND window = state_->window;
    const HWND previous = GetForegroundWindow();
    SetForegroundWindow(window);
    const uint32_t command = static_cast<uint32_t>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, window, nullptr));
    PostMessageW(window, WM_NULL, 0, 0);
    const bool opensSomething = command != 0 && ItemActivates(items, command);
    if (!opensSomething && previous != nullptr && previous != window && IsWindow(previous)) {
        SetForegroundWindow(previous);
    }
    DestroyMenu(menu);
    return command;
}

void Platform::Alert(const std::wstring& text, const std::wstring& title, AlertKind kind) {
    const UINT flags = kind == AlertKind::Error ? MB_ICONERROR : (MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    MessageBoxW(state_->window, text.c_str(), title.c_str(), flags);
}

bool Platform::Confirm(const std::wstring& text, const std::wstring& title) {
    return MessageBoxW(state_->window, text.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES;
}

bool Platform::OpenFolder(const std::wstring& path) {
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool IsAutostartEnabled(const std::wstring& executable) {
    wchar_t buffer[2048] = {};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return false;
    }
    return ToLower(buffer) == ToLower(QuotedExecutable(executable));
}

bool SetAutostart(const std::wstring& executable, bool enabled) {
    if (!enabled) {
        const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    const std::wstring value = QuotedExecutable(executable);
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool RunsFromTemporaryLocation(const std::wstring&) {
    return false;
}

bool SystemUsesDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
                     &value, &size) != ERROR_SUCCESS) {
        return false;
    }
    return value == 0;
}

void ApplyMenuTheme(bool dark) {
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    static const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (uxtheme == nullptr) {
        return;
    }
    const auto setMode = reinterpret_cast<SetPreferredAppMode>(reinterpret_cast<void*>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135))));
    const auto flush = reinterpret_cast<FlushMenuThemes>(reinterpret_cast<void*>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136))));
    if (setMode != nullptr) {
        setMode(dark ? 2 : 3);
    }
    if (flush != nullptr) {
        flush();
    }
}

std::wstring HotkeyLabel(const std::wstring& text) {
    UINT modifiers = 0;
    UINT virtualKey = 0;
    return ParseHotkey(text, modifiers, virtualKey) ? text : std::wstring();
}

Language SystemLanguage() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? Language::Russian : Language::English;
}

}
