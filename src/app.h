#pragma once

#include "monitor.h"
#include "notices.h"
#include "overlay.h"
#include "pets.h"
#include "platform.h"
#include "settings.h"

#include <optional>
#include <string>
#include <vector>

namespace cp {

class App final : private PlatformEvents {
public:
    App();
    int Run(bool startHidden);

private:
    bool Initialize(bool startHidden);
    void Shutdown();

    void OnSessionsChanged() override;
    void OnHookEvent(std::string payload) override;
    void OnShowRequested() override;
    void OnHotkey() override;
    void OnThemeChanged() override;
    void OnTimer(uint32_t id) override;
    void OnTrayClicked() override;
    void OnTrayMenu() override;
    void OnSessionEnding() override;
    void OnCloseRequested() override;

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
    void ToggleAutostart();
    void SyncHookPath();

    void ShowContextMenu(PointI screen);
    void ShowNoticeMenu(const Notice& notice, PointI screen);
    void ShowTrayMenu();
    void ExecuteCommand(uint32_t command);

    std::wstring executable_;
    Settings settings_;
    std::vector<PetInfo> pets_;
    PetSource petSource_ = PetSource::Claude;
    std::wstring activePetKey_;
    Platform platform_;
    Overlay overlay_;
    SessionMonitor monitor_;
    std::optional<Greeting> greeting_;
    std::vector<SessionView> sessions_;
    bool hotkeyRegistered_ = false;
    bool quitting_ = false;
};

}
