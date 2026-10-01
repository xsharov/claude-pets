#include "app.h"

#include "claude_app.h"
#include "hooks.h"
#include "i18n.h"
#include "log.h"
#include "resource.h"
#include "util.h"

#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace cp {

namespace {

constexpr UINT_PTR kExpiryTimer = 10;
constexpr UINT_PTR kSaveTimer = 11;
constexpr UINT_PTR kTopmostTimer = 12;
constexpr UINT_PTR kForegroundTimer = 13;
constexpr int kHotkeyId = 1;
constexpr int kGreetingLifetimeMs = 8000;
constexpr int64_t kFirstRunUnreadWindowMs = 15LL * 60 * 1000;
constexpr UINT_PTR kAliveTimer = 14;

enum Command : UINT {
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

UINT Checked(bool value) {
    return value ? MF_CHECKED : MF_UNCHECKED;
}

int NearestSizeIndex(int width) {
    int best = 0;
    for (int i = 1; i < static_cast<int>(std::size(kSizes)); ++i) {
        if (std::abs(kSizes[i] - width) < std::abs(kSizes[best] - width)) {
            best = i;
        }
    }
    return best;
}

}

int App::Run(HINSTANCE instance, bool startHidden) {
    instance_ = instance;
    executable_ = ExecutablePath();
    if (!Initialize(startHidden)) {
        return 1;
    }
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Shutdown();
    return 0;
}

bool App::Initialize(bool startHidden) {
    InitializeSettings();
    ApplyLanguage();
    icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    OverlayCallbacks callbacks;
    callbacks.openNotice = [this](const Notice& notice) { OpenNotice(notice); };
    callbacks.dismissNotice = [this](const Notice& notice) { DismissNotice(notice); };
    callbacks.noticeMenu = [this](const Notice& notice, POINT screen) { ShowNoticeMenu(notice, screen); };
    callbacks.contextMenu = [this](POINT screen) { ShowContextMenu(screen); };
    callbacks.mascotClicked = [] { ActivateClaude(); };
    callbacks.activityVisibilityRequested = [this](bool visible) { SetActivityVisible(visible); };
    callbacks.positionChanged = [this](const std::wstring& monitorKey, POINT anchor) {
        StorePosition(settings_, monitorKey, anchor);
        ScheduleSave();
    };
    callbacks.messageHandler = [this](UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) { return HandleMessage(message, wParam, lParam, result); };
    if (!overlay_.Create(instance_, std::move(callbacks))) {
        MessageBoxW(nullptr, L"Direct2D initialization failed.", L"Claude Pets", MB_ICONERROR);
        return false;
    }
    ApplyTheme();
    overlay_.SetMascotWidth(settings_.mascotWidth);
    overlay_.SetActivityVisible(settings_.showActivity);
    overlay_.SetLookAtCursor(settings_.lookAtCursor);
    overlay_.PlaceInitial(settings_);
    RefreshPets();
    SelectPet(settings_.petKey, false);
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    RegisterToggleHotkey();
    SyncHookPath();
    const int64_t now = UnixNowMs();
    monitor_.SetUnreadBaseline(settings_.lastAliveMs > 0 ? settings_.lastAliveMs : now - kFirstRunUnreadWindowMs);
    settings_.lastAliveMs = now;
    ScheduleSave();
    monitor_.Start(overlay_.Handle(), kSessionsChangedMessage);
    SetTimer(overlay_.Handle(), kTopmostTimer, 5000, nullptr);
    SetTimer(overlay_.Handle(), kAliveTimer, 5 * 60 * 1000, nullptr);
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
    tray_.Hide();
    if (icon_ != nullptr) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
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
    const HWND window = overlay_.Handle();
    if (hotkeyRegistered_) {
        UnregisterHotKey(window, kHotkeyId);
        hotkeyRegistered_ = false;
    }
    UINT modifiers = 0;
    UINT key = 0;
    if (ParseHotkey(settings_.hotkey, modifiers, key)) {
        hotkeyRegistered_ = RegisterHotKey(window, kHotkeyId, modifiers | MOD_NOREPEAT, key) != FALSE;
    }
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
        MessageBoxW(overlay_.Handle(), message.c_str(), L"Claude Pets", MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }
    activePetKey_ = pet->key;
    if (LogEnabled()) {
        const D2D1_RECT_F& bounds = graphics.SpriteContentBounds();
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
    const HWND window = overlay_.Handle();
    if (nextExpiry == LLONG_MAX) {
        KillTimer(window, kExpiryTimer);
        return;
    }
    const int64_t delay = std::clamp<int64_t>(nextExpiry - now + 50, 100, 60LL * 60 * 1000);
    SetTimer(window, kExpiryTimer, static_cast<UINT>(delay), nullptr);
}

void App::ScheduleSave() {
    SetTimer(overlay_.Handle(), kSaveTimer, 800, nullptr);
}

void App::SaveNow() {
    KillTimer(overlay_.Handle(), kSaveTimer);
    settings_.lastAliveMs = UnixNowMs();
    SaveSettings(settings_);
}

bool App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    switch (message) {
    case kSessionsChangedMessage:
        sessions_ = monitor_.Snapshot();
        RebuildNotices();
        result = 0;
        return true;
    case kShowPetMessage:
        ShowPet();
        result = 0;
        return true;
    case kTrayMessage:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            ShowPet();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
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
        monitor_.SubmitHookEvent(std::string(static_cast<const char*>(data->lpData), data->cbData));
        result = TRUE;
        return true;
    }
    case WM_HOTKEY:
        if (wParam == kHotkeyId) {
            TogglePet();
            result = 0;
            return true;
        }
        return false;
    case WM_TIMER:
        if (wParam == kExpiryTimer || wParam == kSaveTimer || wParam == kTopmostTimer || wParam == kForegroundTimer || wParam == kAliveTimer) {
            HandleTimer(wParam);
            result = 0;
            return true;
        }
        return false;
    case WM_SETTINGCHANGE:
        if (lParam != 0 && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            ApplyTheme();
        }
        return false;
    case WM_QUERYENDSESSION:
        SaveNow();
        result = TRUE;
        return true;
    case WM_ENDSESSION:
        if (wParam != 0) {
            SaveNow();
        }
        return false;
    case WM_CLOSE:
        Quit();
        result = 0;
        return true;
    case WM_DESTROY:
        if (!quitting_) {
            PostQuitMessage(0);
        }
        return false;
    default:
        if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
            tray_.Restore();
            result = 0;
            return true;
        }
        return false;
    }
}

void App::HandleTimer(UINT_PTR id) {
    const HWND window = overlay_.Handle();
    switch (id) {
    case kExpiryTimer:
        KillTimer(window, kExpiryTimer);
        RebuildNotices();
        break;
    case kSaveTimer:
        SaveNow();
        break;
    case kTopmostTimer:
        if (overlay_.IsShown()) {
            SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        break;
    case kAliveTimer:
        SaveNow();
        break;
    case kForegroundTimer:
        KillTimer(window, kForegroundTimer);
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
    SetTimer(overlay_.Handle(), kForegroundTimer, 1200, nullptr);
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
    tray_.Hide();
}

void App::HidePet() {
    overlay_.Hide();
    tray_.Show(overlay_.Handle(), kTrayMessage, icon_, Text().trayTip);
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
    tray_.Hide();
    if (hotkeyRegistered_) {
        UnregisterHotKey(overlay_.Handle(), kHotkeyId);
        hotkeyRegistered_ = false;
    }
    overlay_.Destroy();
    PostQuitMessage(0);
}

void App::ToggleHooks() {
    const Strings& text = Text();
    const HWND owner = overlay_.Handle();
    const HookState state = QueryHookState(executable_);
    if (state == HookState::Unreadable) {
        MessageBoxW(owner, text.hooksFailed, text.hooksInstallTitle, MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        return;
    }
    if (state == HookState::Installed || state == HookState::Outdated) {
        if (MessageBoxW(owner, text.hooksRemoveBody, text.hooksInstallTitle, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES && !UninstallHooks()) {
            MessageBoxW(owner, text.hooksFailed, text.hooksInstallTitle, MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        }
        return;
    }
    if (MessageBoxW(owner, text.hooksInstallBody, text.hooksInstallTitle, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES &&
        !InstallHooks(executable_)) {
        MessageBoxW(owner, text.hooksFailed, text.hooksInstallTitle, MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }
}

void App::SyncHookPath() {
    if (QueryHookState(executable_) == HookState::Outdated) {
        InstallHooks(executable_);
    }
}

UINT App::TrackMenu(HMENU menu, POINT screen) {
    const HWND window = overlay_.Handle();
    const HWND previous = GetForegroundWindow();
    SetForegroundWindow(window);
    const UINT command = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, window, nullptr));
    PostMessageW(window, WM_NULL, 0, 0);
    const bool opensSomething = command == kCommandOpenClaude || command == kCommandOpenPetsFolder || command == kCommandOpenSession || command == kCommandHooks ||
                                command == kCommandClose;
    if (!opensSomething && previous != nullptr && previous != window && IsWindow(previous)) {
        SetForegroundWindow(previous);
    }
    return command;
}

void App::ShowContextMenu(POINT screen) {
    RefreshPets();
    const Strings& text = Text();
    HMENU menu = CreatePopupMenu();
    HMENU petMenu = CreatePopupMenu();
    if (pets_.empty()) {
        AppendMenuW(petMenu, MF_STRING | MF_GRAYED, 0, text.menuNoPets);
    }
    for (size_t i = 0; i < pets_.size() && i < 400; ++i) {
        AppendMenuW(petMenu, MF_STRING | Checked(pets_[i].key == activePetKey_), kCommandPetBase + static_cast<UINT>(i), pets_[i].displayName.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(petMenu), text.menuPet);
    HMENU folderMenu = CreatePopupMenu();
    const std::wstring home = HomeDirectory();
    for (const PetSource source : {PetSource::Claude, PetSource::Codex}) {
        std::wstring folder = PetFolder(source);
        if (!home.empty() && StartsWith(ToLower(folder), ToLower(home))) {
            folder = L"~" + folder.substr(home.size());
        }
        const size_t count = source == petSource_ ? pets_.size() : DiscoverPets(source).size();
        const std::wstring label = folder + L"  (" + std::to_wstring(count) + L")";
        AppendMenuW(folderMenu, MF_STRING | Checked(source == petSource_), source == PetSource::Codex ? kCommandPetsCodex : kCommandPetsClaude, label.c_str());
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(folderMenu), text.menuPetsFolder);
    HMENU sizeMenu = CreatePopupMenu();
    const wchar_t* sizeLabels[] = {text.sizeSmall, text.sizeMedium, text.sizeLarge, text.sizeHuge};
    const int currentSize = NearestSizeIndex(settings_.mascotWidth);
    for (int i = 0; i < static_cast<int>(std::size(kSizes)); ++i) {
        AppendMenuW(sizeMenu, MF_STRING | Checked(i == currentSize), kCommandSizeBase + static_cast<UINT>(i), sizeLabels[i]);
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sizeMenu), text.menuSize);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | Checked(settings_.showActivity), kCommandShowActivity, text.menuShowActivity);
    AppendMenuW(menu, MF_STRING | Checked(settings_.lookAtCursor), kCommandLookAtCursor, text.menuLookAtCursor);
    if (!settings_.muted.empty()) {
        AppendMenuW(menu, MF_STRING, kCommandUnmuteAll, text.menuUnmuteAll);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandOpenClaude, text.menuOpenClaude);
    AppendMenuW(menu, MF_STRING, kCommandOpenPetsFolder, text.menuOpenPetsFolder);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | Checked(IsAutostartEnabled(executable_)), kCommandAutostart, text.menuStartWithWindows);
    const HookState hooks = QueryHookState(executable_);
    AppendMenuW(menu, MF_STRING | Checked(hooks == HookState::Installed || hooks == HookState::Outdated), kCommandHooks, text.menuHooks);
    HMENU languageMenu = CreatePopupMenu();
    AppendMenuW(languageMenu, MF_STRING | Checked(settings_.language != L"en" && settings_.language != L"ru"), kCommandLanguageSystem,
                CurrentLanguage() == Language::Russian ? L"Как в системе" : L"System default");
    AppendMenuW(languageMenu, MF_STRING | Checked(settings_.language == L"en"), kCommandLanguageEnglish, L"English");
    AppendMenuW(languageMenu, MF_STRING | Checked(settings_.language == L"ru"), kCommandLanguageRussian, L"Русский");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(languageMenu), text.menuLanguage);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::wstring hideLabel = text.menuHide;
    const std::wstring hotkey = HotkeyLabel(settings_.hotkey);
    if (hotkeyRegistered_ && !hotkey.empty()) {
        hideLabel += L"\t" + hotkey;
    }
    AppendMenuW(menu, MF_STRING, kCommandHide, hideLabel.c_str());
    AppendMenuW(menu, MF_STRING, kCommandClose, text.menuClose);
    const UINT command = TrackMenu(menu, screen);
    DestroyMenu(menu);
    if (command != 0) {
        ExecuteCommand(command);
    }
}

void App::ShowNoticeMenu(const Notice& notice, POINT screen) {
    const Strings& text = Text();
    HMENU menu = CreatePopupMenu();
    if (notice.kind == NoticeKind::Session) {
        AppendMenuW(menu, MF_STRING, kCommandOpenSession, text.menuOpenSession);
    }
    AppendMenuW(menu, MF_STRING, kCommandDismiss, text.menuDismiss);
    if (notice.kind == NoticeKind::Session) {
        AppendMenuW(menu, MF_STRING, kCommandMute, text.menuMute);
    }
    if (!settings_.muted.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCommandUnmuteAll, text.menuUnmuteAll);
    }
    const UINT command = TrackMenu(menu, screen);
    DestroyMenu(menu);
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
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCommandTrayShow, text.trayShow);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandClose, text.menuClose);
    POINT cursor = {};
    GetCursorPos(&cursor);
    const UINT command = TrackMenu(menu, cursor);
    DestroyMenu(menu);
    if (command != 0) {
        ExecuteCommand(command);
    }
}

void App::ExecuteCommand(UINT command) {
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
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
        SetAutostart(executable_, !IsAutostartEnabled(executable_));
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
