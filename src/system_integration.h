#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

namespace cp {

bool IsAutostartEnabled(const std::wstring& executable);
bool SetAutostart(const std::wstring& executable, bool enabled);
bool SystemUsesDarkTheme();
void ApplyMenuTheme(bool dark);
bool ParseHotkey(const std::wstring& text, UINT& modifiers, UINT& virtualKey);
std::wstring HotkeyLabel(const std::wstring& text);

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    void Show(HWND owner, UINT callbackMessage, HICON icon, const std::wstring& tooltip);
    void Hide();
    void Restore();
    bool Visible() const { return visible_; }

private:
    NOTIFYICONDATAW data_ = {};
    bool visible_ = false;
};

}
