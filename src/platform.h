#pragma once

#include "geometry.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cp {

class OverlayWindow;

struct MenuItem {
    enum class Kind : uint8_t { Command, Separator, Submenu };

    Kind kind = Kind::Command;
    uint32_t command = 0;
    std::wstring label;
    std::wstring shortcut;
    bool checked = false;
    bool enabled = true;
    bool activates = false;
    std::vector<MenuItem> children;
};

enum class AlertKind : uint8_t { Error, Warning };

class PlatformEvents {
public:
    virtual void OnSessionsChanged() = 0;
    virtual void OnHookEvent(std::string payload) = 0;
    virtual void OnShowRequested() = 0;
    virtual void OnHotkey() = 0;
    virtual void OnThemeChanged() = 0;
    virtual void OnTimer(uint32_t id) = 0;
    virtual void OnTrayClicked() = 0;
    virtual void OnTrayMenu() = 0;
    virtual void OnSessionEnding() = 0;
    virtual void OnCloseRequested() = 0;

protected:
    ~PlatformEvents() = default;
};

class Platform {
public:
    explicit Platform(PlatformEvents& events);
    ~Platform();
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    bool Attach(OverlayWindow& window);
    void Run();
    void Quit();
    void PostSessionsChanged();

    void SetTimer(uint32_t id, uint32_t delayMs);
    void KillTimer(uint32_t id);

    bool RegisterHotkey(const std::wstring& text);
    void UnregisterHotkey();

    void ShowTray(const std::wstring& tooltip);
    void HideTray();

    uint32_t ShowMenu(const std::vector<MenuItem>& items, PointI screen);
    void Alert(const std::wstring& text, const std::wstring& title, AlertKind kind);
    bool Confirm(const std::wstring& text, const std::wstring& title);
    bool OpenFolder(const std::wstring& path);

private:
    struct State;

    PlatformEvents& events_;
    std::unique_ptr<State> state_;
};

bool SystemUsesDarkTheme();
void ApplyMenuTheme(bool dark);
std::wstring HotkeyLabel(const std::wstring& text);
bool IsAutostartEnabled(const std::wstring& executable);
bool SetAutostart(const std::wstring& executable, bool enabled);
bool RunsFromTemporaryLocation(const std::wstring& executable);

}
