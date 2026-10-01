#pragma once

#include "monitor.h"
#include "notices.h"
#include "overlay.h"
#include "pets.h"
#include "settings.h"
#include "system_integration.h"

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace cp {

inline constexpr UINT kSessionsChangedMessage = WM_APP + 1;
inline constexpr UINT kShowPetMessage = WM_APP + 2;
inline constexpr UINT kTrayMessage = WM_APP + 3;

class App {
public:
    int Run(HINSTANCE instance, bool startHidden);

private:
    bool Initialize(bool startHidden);
    void Shutdown();
    bool HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
    void HandleTimer(UINT_PTR id);

    void InitializeSettings();
    void ApplyLanguage();
    void ApplyTheme();
    void RegisterToggleHotkey();
    void RefreshPets();
    void SelectPet(const std::wstring& key, bool persist);
    void RebuildNotices();
    void ScheduleSave();
    void SaveNow();

    void OpenNotice(const Notice& notice);
    void DismissNotice(const Notice& notice);
    void SetActivityVisible(bool visible);
    void ShowPet();
    void HidePet();
    void TogglePet();
    void Quit();
    void ToggleHooks();
    void SyncHookPath();

    void ShowContextMenu(POINT screen);
    void ShowNoticeMenu(const Notice& notice, POINT screen);
    void ShowTrayMenu();
    UINT TrackMenu(HMENU menu, POINT screen);
    void ExecuteCommand(UINT command);

    HINSTANCE instance_ = nullptr;
    std::wstring executable_;
    Settings settings_;
    std::vector<PetInfo> pets_;
    PetSource petSource_ = PetSource::Claude;
    std::wstring activePetKey_;
    Overlay overlay_;
    SessionMonitor monitor_;
    TrayIcon tray_;
    HICON icon_ = nullptr;
    std::optional<Greeting> greeting_;
    std::vector<SessionView> sessions_;
    UINT taskbarCreatedMessage_ = 0;
    bool hotkeyRegistered_ = false;
    bool quitting_ = false;
};

}
