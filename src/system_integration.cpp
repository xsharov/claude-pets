#include "system_integration.h"

#include "util.h"

#include <vector>

namespace cp {

namespace {

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

bool SystemUsesDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
                     &value, &size) != ERROR_SUCCESS) {
        return false;
    }
    return value == 0;
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

std::wstring HotkeyLabel(const std::wstring& text) {
    UINT modifiers = 0;
    UINT virtualKey = 0;
    return ParseHotkey(text, modifiers, virtualKey) ? text : std::wstring();
}

TrayIcon::~TrayIcon() {
    Hide();
}

void TrayIcon::Show(HWND owner, UINT callbackMessage, HICON icon, const std::wstring& tooltip) {
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

void TrayIcon::Hide() {
    if (!visible_) {
        return;
    }
    Shell_NotifyIconW(NIM_DELETE, &data_);
    visible_ = false;
}

void TrayIcon::Restore() {
    if (visible_) {
        visible_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    }
}

}
