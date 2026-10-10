#include "app.h"

#include "claude_app.h"
#include "hooks.h"
#include "i18n.h"
#include "log.h"
#include "util.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <iterator>

namespace cp {

namespace {

constexpr uint32_t kExpiryTimer = 10;
constexpr uint32_t kSaveTimer = 11;
constexpr uint32_t kTopmostTimer = 12;
constexpr uint32_t kForegroundTimer = 13;
constexpr uint32_t kAliveTimer = 14;
constexpr int kGreetingLifetimeMs = 8000;
constexpr int64_t kFirstRunUnreadWindowMs = 15LL * 60 * 1000;

#if defined(_WIN32)
constexpr wchar_t kGraphicsFailed[] = L"Direct2D initialization failed.";
#else
constexpr wchar_t kGraphicsFailed[] = L"Graphics initialization failed.";
#endif

enum Command : uint32_t {
    kCommandPetBase = 1000,
    kCommandSizeBase = 1500,
    kCommandShowActivity = 1600,
    kCommandLookAtCursor,
    kCommandOpenClaude,
    kCommandOpenPetsFolder,
    kCommandAutostart,
    kCommandHooks,
    kCommandHide,
    kCommandClose,
    kCommandLanguageSystem,
    kCommandLanguageEnglish,
    kCommandLanguageRussian,
    kCommandOpenSession,
    kCommandDismiss,
    kCommandMute,
    kCommandUnmuteAll,
    kCommandTrayShow,
    kCommandPetsClaude,
    kCommandPetsCodex,
};

constexpr int kSizes[] = {80, 112, 160, 224};

int NearestSizeIndex(int width) {
    int best = 0;
    for (int i = 1; i < static_cast<int>(std::size(kSizes)); ++i) {
        if (std::abs(kSizes[i] - width) < std::abs(kSizes[best] - width)) {
            best = i;
        }
    }
    return best;
}

bool Activates(uint32_t command) {
    return command == kCommandOpenClaude || command == kCommandOpenPetsFolder || command == kCommandOpenSession || command == kCommandHooks ||
           command == kCommandClose;
}

MenuItem Item(uint32_t command, std::wstring label, bool checked = false) {
    MenuItem item;
    item.command = command;
    item.label = std::move(label);
    item.checked = checked;
    item.activates = Activates(command);
    return item;
}

MenuItem Separator() {
    MenuItem item;
    item.kind = MenuItem::Kind::Separator;
    return item;
}

MenuItem Submenu(std::wstring label) {
    MenuItem item;
    item.kind = MenuItem::Kind::Submenu;
    item.label = std::move(label);
    return item;
}

}

App::App() : platform_(*this) {}

int App::Run(bool startHidden) {
    executable_ = ExecutablePath();
    if (!Initialize(startHidden)) {
        return 1;
    }
    platform_.Run();
    Shutdown();
    return 0;
}

bool App::Initialize(bool startHidden) {
    InitializeSettings();
    ApplyLanguage();
    OverlayCallbacks callbacks;
    callbacks.openNotice = [this](const Notice& notice) { OpenNotice(notice); };
    callbacks.dismissNotice = [this](const Notice& notice) { DismissNotice(notice); };
    callbacks.noticeMenu = [this](const Notice& notice, PointI screen) { ShowNoticeMenu(notice, screen); };
    callbacks.contextMenu = [this](PointI screen) { ShowContextMenu(screen); };
    callbacks.mascotClicked = [] { ActivateClaude(); };
    callbacks.activityVisibilityRequested = [this](bool visible) { SetActivityVisible(visible); };
    callbacks.positionChanged = [this](const std::wstring& monitorKey, PointI anchor) {
        StorePosition(settings_, monitorKey, anchor);
        ScheduleSave();
    };
    if (!overlay_.Create(std::move(callbacks))) {
        platform_.Alert(kGraphicsFailed, L"Claude Pets", AlertKind::Error);
        return false;
    }
    if (!platform_.Attach(overlay_.Window())) {
        return false;
    }
    ApplyTheme();
    overlay_.SetMascotWidth(settings_.mascotWidth);
    overlay_.SetActivityVisible(settings_.showActivity);
    overlay_.SetLookAtCursor(settings_.lookAtCursor);
    overlay_.PlaceInitial(settings_);
    RefreshPets();
    SelectPet(settings_.petKey, false);
    RegisterToggleHotkey();
    SyncHookPath();
    const int64_t now = UnixNowMs();
    monitor_.SetUnreadBaseline(settings_.lastAliveMs > 0 ? settings_.lastAliveMs : now - kFirstRunUnreadWindowMs);
    settings_.lastAliveMs = now;
    ScheduleSave();
    monitor_.Start([this] { platform_.PostSessionsChanged(); });
    platform_.SetTimer(kTopmostTimer, 5000);
    platform_.SetTimer(kAliveTimer, 5 * 60 * 1000);
    if (startHidden) {
        HidePet();
    } else {
        ShowPet();
    }
    RebuildNotices();
    return true;
}

void App::Shutdown() {
    monitor_.Stop();
    platform_.HideTray();
}

void App::InitializeSettings() {
    settings_ = LoadSettings();
    if (!settings_.existed) {
        const CodexPetPreference preference = ReadCodexPetPreference();
        if (!preference.petKey.empty()) {
            settings_.petKey = preference.petKey;
        }
        if (preference.mascotWidth > 0) {
            settings_.mascotWidth = preference.mascotWidth;
        }
    }
    settings_.mascotWidth = std::clamp(settings_.mascotWidth, kSizes[0], kSizes[std::size(kSizes) - 1]);
}

void App::ApplyLanguage() {
    if (settings_.language == L"en") {
        SetLanguage(Language::English);
    } else if (settings_.language == L"ru") {
        SetLanguage(Language::Russian);
    } else {
        SetLanguage(SystemLanguage());
    }
}

void App::ApplyTheme() {
    const bool dark = settings_.theme == L"dark" || (settings_.theme != L"light" && SystemUsesDarkTheme());
    ApplyMenuTheme(dark);
    overlay_.SetDarkTheme(dark);
}

void App::RegisterToggleHotkey() {
    if (hotkeyRegistered_) {
        platform_.UnregisterHotkey();
        hotkeyRegistered_ = false;
    }
    hotkeyRegistered_ = platform_.RegisterHotkey(settings_.hotkey);
}

void App::RefreshPets() {
    petSource_ = ResolvePetSource(settings_.petsFolder);
    pets_ = DiscoverPets(petSource_);
}

void App::SelectPet(const std::wstring& key, bool persist) {
    const PetInfo* pet = FindPet(pets_, key);
    if (pet == nullptr) {
        pet = FindPet(pets_, std::wstring(PetSourceKey(petSource_)) + L":" + PetFolderName(key));
    }
    if (pet == nullptr && !pets_.empty()) {
        pet = &pets_.front();
    }
    Renderer& graphics = overlay_.Graphics();
    if (pet == nullptr) {
        graphics.UnloadSpritesheet();
        activePetKey_.clear();
        overlay_.PetChanged();
        return;
    }
    std::wstring error;
    bool loaded = graphics.LoadSpritesheet(pet->spritesheet, pet->spriteVersion, error);
    if (!loaded && !pet->fallbackSpritesheet.empty()) {
        std::wstring fallbackError;
        loaded = graphics.LoadSpritesheet(pet->fallbackSpritesheet, pet->spriteVersion, fallbackError);
    }
    if (!loaded) {
        const Strings& text = Text();
        const std::wstring message = error == L"codec" ? std::wstring(text.codecMissing) : Format(text.spriteFailed, pet->spritesheet);
        platform_.Alert(message, L"Claude Pets", AlertKind::Warning);
    }
    activePetKey_ = pet->key;
    if (LogEnabled()) {
        const RectF& bounds = graphics.SpriteContentBounds();
        LogLine("pet " + Narrow(pet->key) + " rows=" + std::to_string(graphics.SpriteRows()) + " bounds=" + std::to_string(bounds.left) + "," + std::to_string(bounds.top) + "," +
                std::to_string(bounds.right) + "," + std::to_string(bounds.bottom));
    }
    if (persist || settings_.petKey != pet->key) {
        settings_.petKey = pet->key;
        ScheduleSave();
    }
    if (std::find(settings_.greetedPets.begin(), settings_.greetedPets.end(), pet->key) == settings_.greetedPets.end()) {
        greeting_ = Greeting{pet->displayName, UnixNowMs()};
        settings_.greetedPets.push_back(pet->key);
        ScheduleSave();
    }
    overlay_.PetChanged();
    RebuildNotices();
}

void App::RebuildNotices() {
    const int64_t now = UnixNowMs();
    if (greeting_.has_value() && now >= greeting_->startedAtMs + kGreetingLifetimeMs) {
        greeting_.reset();
    }
    std::vector<Notice> notices = BuildNotices(sessions_, settings_, greeting_, now);
    const int64_t nextExpiry = NextExpiryMs(notices);
    const PetAnim anim = MascotAnimFor(notices);
    overlay_.SetNotices(std::move(notices), anim);
    if (nextExpiry == LLONG_MAX) {
        platform_.KillTimer(kExpiryTimer);
        return;
    }
    const int64_t delay = std::clamp<int64_t>(nextExpiry - now + 50, 100, 60LL * 60 * 1000);
    platform_.SetTimer(kExpiryTimer, static_cast<uint32_t>(delay));
}

void App::ScheduleSave() {
    platform_.SetTimer(kSaveTimer, 800);
}

void App::SaveNow() {
    platform_.KillTimer(kSaveTimer);
    settings_.lastAliveMs = UnixNowMs();
    SaveSettings(settings_);
}

void App::OnSessionsChanged() {
    sessions_ = monitor_.Snapshot();
    RebuildNotices();
}

void App::OnHookEvent(std::string payload) {
    monitor_.SubmitHookEvent(std::move(payload));
}

void App::OnShowRequested() {
    ShowPet();
}

void App::OnHotkey() {
    TogglePet();
}

void App::OnThemeChanged() {
    ApplyTheme();
}

void App::OnTrayClicked() {
    ShowPet();
}

void App::OnTrayMenu() {
    ShowTrayMenu();
}

void App::OnSessionEnding() {
    SaveNow();
}

void App::OnCloseRequested() {
    Quit();
}

void App::OnTimer(uint32_t id) {
    switch (id) {
    case kExpiryTimer:
        platform_.KillTimer(kExpiryTimer);
        RebuildNotices();
        break;
    case kSaveTimer:
        SaveNow();
        break;
    case kTopmostTimer:
        if (overlay_.IsShown()) {
            overlay_.Window().EnsureTopmost();
        }
        break;
    case kAliveTimer:
        SaveNow();
        break;
    case kForegroundTimer:
        platform_.KillTimer(kForegroundTimer);
        if (!IsClaudeForeground()) {
            BringClaudeToFront();
        }
        break;
    default:
        break;
    }
}

void App::OpenNotice(const Notice& notice) {
    if (notice.kind == NoticeKind::Greeting) {
        greeting_.reset();
        RebuildNotices();
        ActivateClaude();
        return;
    }
    const bool acknowledge = notice.status == SessionStatus::Review || notice.status == SessionStatus::Failed ||
                             (notice.status == SessionStatus::Waiting && notice.completedTurn);
    if (acknowledge) {
        Dismiss(settings_, notice.id, notice.turnKey);
        monitor_.MarkTurnRead(notice.id, notice.turnKey);
        ScheduleSave();
        RebuildNotices();
    }
    OpenClaudeSession(notice.id);
    platform_.SetTimer(kForegroundTimer, 1200);
}

void App::DismissNotice(const Notice& notice) {
    if (notice.kind == NoticeKind::Greeting) {
        greeting_.reset();
    } else {
        Dismiss(settings_, notice.id, notice.turnKey);
        ScheduleSave();
    }
    RebuildNotices();
}

void App::SetActivityVisible(bool visible) {
    settings_.showActivity = visible;
    overlay_.SetActivityVisible(visible);
    ScheduleSave();
}

void App::ShowPet() {
    overlay_.Show();
    platform_.HideTray();
}

void App::HidePet() {
    overlay_.Hide();
    platform_.ShowTray(Text().trayTip);
}

void App::TogglePet() {
    if (overlay_.IsShown()) {
        HidePet();
    } else {
        ShowPet();
    }
}

void App::Quit() {
    if (quitting_) {
        return;
    }
    quitting_ = true;
    SaveNow();
    monitor_.Stop();
    platform_.HideTray();
    if (hotkeyRegistered_) {
        platform_.UnregisterHotkey();
        hotkeyRegistered_ = false;
    }
    platform_.Quit();
    overlay_.Destroy();
}

void App::ToggleHooks() {
    const Strings& text = Text();
    const HookState state = QueryHookState(executable_);
    if (state == HookState::Unreadable) {
        platform_.Alert(text.hooksFailed, text.hooksInstallTitle, AlertKind::Warning);
        return;
    }
    if (state == HookState::Installed || state == HookState::Outdated) {
        if (platform_.Confirm(text.hooksRemoveBody, text.hooksInstallTitle) && !UninstallHooks()) {
            platform_.Alert(text.hooksFailed, text.hooksInstallTitle, AlertKind::Warning);
        }
        return;
    }
    if (RunsFromTemporaryLocation(executable_)) {
        platform_.Alert(text.appTranslocated, text.hooksInstallTitle, AlertKind::Warning);
        return;
    }
    if (platform_.Confirm(text.hooksInstallBody, text.hooksInstallTitle) && !InstallHooks(executable_)) {
        platform_.Alert(text.hooksFailed, text.hooksInstallTitle, AlertKind::Warning);
    }
}

void App::ToggleAutostart() {
    const bool enable = !IsAutostartEnabled(executable_);
    if (enable && RunsFromTemporaryLocation(executable_)) {
        platform_.Alert(Text().appTranslocated, L"Claude Pets", AlertKind::Warning);
        return;
    }
    SetAutostart(executable_, enable);
}

void App::SyncHookPath() {
    if (!RunsFromTemporaryLocation(executable_) && QueryHookState(executable_) == HookState::Outdated) {
        InstallHooks(executable_);
    }
}

void App::ShowContextMenu(PointI screen) {
    RefreshPets();
    const Strings& text = Text();
    std::vector<MenuItem> menu;
    MenuItem petMenu = Submenu(text.menuPet);
    if (pets_.empty()) {
        MenuItem empty = Item(0, text.menuNoPets);
        empty.enabled = false;
        petMenu.children.push_back(std::move(empty));
    }
    for (size_t i = 0; i < pets_.size() && i < 400; ++i) {
        petMenu.children.push_back(Item(kCommandPetBase + static_cast<uint32_t>(i), pets_[i].displayName, pets_[i].key == activePetKey_));
    }
    menu.push_back(std::move(petMenu));
    MenuItem folderMenu = Submenu(text.menuPetsFolder);
    const std::wstring home = HomeDirectory();
    for (const PetSource source : {PetSource::Claude, PetSource::Codex}) {
        std::wstring folder = PetFolder(source);
        if (!home.empty() && StartsWith(ToLower(folder), ToLower(home))) {
            folder = L"~" + folder.substr(home.size());
        }
        const size_t count = source == petSource_ ? pets_.size() : DiscoverPets(source).size();
        const std::wstring label = folder + L"  (" + std::to_wstring(count) + L")";
        folderMenu.children.push_back(Item(source == PetSource::Codex ? kCommandPetsCodex : kCommandPetsClaude, label, source == petSource_));
    }
    menu.push_back(std::move(folderMenu));
    MenuItem sizeMenu = Submenu(text.menuSize);
    const wchar_t* sizeLabels[] = {text.sizeSmall, text.sizeMedium, text.sizeLarge, text.sizeHuge};
    const int currentSize = NearestSizeIndex(settings_.mascotWidth);
    for (int i = 0; i < static_cast<int>(std::size(kSizes)); ++i) {
        sizeMenu.children.push_back(Item(kCommandSizeBase + static_cast<uint32_t>(i), sizeLabels[i], i == currentSize));
    }
    menu.push_back(std::move(sizeMenu));
    menu.push_back(Separator());
    menu.push_back(Item(kCommandShowActivity, text.menuShowActivity, settings_.showActivity));
    menu.push_back(Item(kCommandLookAtCursor, text.menuLookAtCursor, settings_.lookAtCursor));
    if (!settings_.muted.empty()) {
        menu.push_back(Item(kCommandUnmuteAll, text.menuUnmuteAll));
    }
    menu.push_back(Separator());
    menu.push_back(Item(kCommandOpenClaude, text.menuOpenClaude));
    menu.push_back(Item(kCommandOpenPetsFolder, text.menuOpenPetsFolder));
    menu.push_back(Separator());
    menu.push_back(Item(kCommandAutostart, text.menuAutostart, IsAutostartEnabled(executable_)));
    const HookState hooks = QueryHookState(executable_);
    menu.push_back(Item(kCommandHooks, text.menuHooks, hooks == HookState::Installed || hooks == HookState::Outdated));
    MenuItem languageMenu = Submenu(text.menuLanguage);
    languageMenu.children.push_back(Item(kCommandLanguageSystem, CurrentLanguage() == Language::Russian ? L"Как в системе" : L"System default",
                                         settings_.language != L"en" && settings_.language != L"ru"));
    languageMenu.children.push_back(Item(kCommandLanguageEnglish, L"English", settings_.language == L"en"));
    languageMenu.children.push_back(Item(kCommandLanguageRussian, L"Русский", settings_.language == L"ru"));
    menu.push_back(std::move(languageMenu));
    menu.push_back(Separator());
    MenuItem hide = Item(kCommandHide, text.menuHide);
    if (hotkeyRegistered_) {
        hide.shortcut = HotkeyLabel(settings_.hotkey);
    }
    menu.push_back(std::move(hide));
    menu.push_back(Item(kCommandClose, text.menuClose));
    const uint32_t command = platform_.ShowMenu(menu, screen);
    if (command != 0) {
        ExecuteCommand(command);
    }
}

void App::ShowNoticeMenu(const Notice& notice, PointI screen) {
    const Strings& text = Text();
    std::vector<MenuItem> menu;
    if (notice.kind == NoticeKind::Session) {
        menu.push_back(Item(kCommandOpenSession, text.menuOpenSession));
    }
    menu.push_back(Item(kCommandDismiss, text.menuDismiss));
    if (notice.kind == NoticeKind::Session) {
        menu.push_back(Item(kCommandMute, text.menuMute));
    }
    if (!settings_.muted.empty()) {
        menu.push_back(Separator());
        menu.push_back(Item(kCommandUnmuteAll, text.menuUnmuteAll));
    }
    const uint32_t command = platform_.ShowMenu(menu, screen);
    switch (command) {
    case kCommandOpenSession:
        OpenNotice(notice);
        break;
    case kCommandDismiss:
        DismissNotice(notice);
        break;
    case kCommandMute:
        settings_.muted.insert(notice.id);
        ScheduleSave();
        RebuildNotices();
        break;
    case kCommandUnmuteAll:
        settings_.muted.clear();
        ScheduleSave();
        RebuildNotices();
        break;
    default:
        break;
    }
}

void App::ShowTrayMenu() {
    const Strings& text = Text();
    std::vector<MenuItem> menu;
    menu.push_back(Item(kCommandTrayShow, text.trayShow));
    menu.push_back(Separator());
    menu.push_back(Item(kCommandClose, text.menuClose));
    const uint32_t command = platform_.ShowMenu(menu, CursorPosition());
    if (command != 0) {
        ExecuteCommand(command);
    }
}

void App::ExecuteCommand(uint32_t command) {
    if (command >= kCommandPetBase && command < kCommandPetBase + pets_.size()) {
        SelectPet(pets_[command - kCommandPetBase].key, true);
        return;
    }
    if (command >= kCommandSizeBase && command < kCommandSizeBase + std::size(kSizes)) {
        settings_.mascotWidth = kSizes[command - kCommandSizeBase];
        overlay_.SetMascotWidth(settings_.mascotWidth);
        StorePosition(settings_, overlay_.MonitorKey(), overlay_.Anchor());
        ScheduleSave();
        return;
    }
    switch (command) {
    case kCommandShowActivity:
        SetActivityVisible(!settings_.showActivity);
        break;
    case kCommandLookAtCursor:
        settings_.lookAtCursor = !settings_.lookAtCursor;
        overlay_.SetLookAtCursor(settings_.lookAtCursor);
        ScheduleSave();
        break;
    case kCommandOpenClaude:
        ActivateClaude();
        break;
    case kCommandOpenPetsFolder: {
        const std::wstring folder = PetFolder(petSource_);
        EnsureDirectory(folder);
        platform_.OpenFolder(folder);
        break;
    }
    case kCommandPetsClaude:
    case kCommandPetsCodex:
        settings_.petsFolder = command == kCommandPetsCodex ? L"codex" : L"claude";
        RefreshPets();
        SelectPet(settings_.petKey, true);
        ScheduleSave();
        break;
    case kCommandAutostart:
        ToggleAutostart();
        break;
    case kCommandHooks:
        ToggleHooks();
        break;
    case kCommandLanguageSystem:
    case kCommandLanguageEnglish:
    case kCommandLanguageRussian:
        settings_.language = command == kCommandLanguageEnglish ? L"en" : (command == kCommandLanguageRussian ? L"ru" : L"auto");
        ApplyLanguage();
        RebuildNotices();
        ScheduleSave();
        break;
    case kCommandHide:
        HidePet();
        break;
    case kCommandClose:
        Quit();
        break;
    case kCommandTrayShow:
        ShowPet();
        break;
    case kCommandUnmuteAll:
        settings_.muted.clear();
        ScheduleSave();
        RebuildNotices();
        break;
    default:
        break;
    }
}

}
