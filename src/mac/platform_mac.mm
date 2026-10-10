#include "platform.h"

#include "i18n.h"
#include "mac/mac_util.h"
#include "overlay_window.h"
#include "util.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <ServiceManagement/ServiceManagement.h>

#include <atomic>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <unordered_map>
#include <vector>

namespace cp {

namespace {

constexpr UInt32 kHotkeySignature = 'CPet';
constexpr UInt32 kHotkeyIdentifier = 1;

bool g_darkMenus = false;

struct Hotkey {
    UInt32 keyCode = 0;
    UInt32 modifiers = 0;
    std::wstring key;
};

struct KeyName {
    const wchar_t* name;
    UInt32 code;
};

constexpr KeyName kKeys[] = {
    {L"a", kVK_ANSI_A}, {L"b", kVK_ANSI_B}, {L"c", kVK_ANSI_C}, {L"d", kVK_ANSI_D}, {L"e", kVK_ANSI_E}, {L"f", kVK_ANSI_F}, {L"g", kVK_ANSI_G},
    {L"h", kVK_ANSI_H}, {L"i", kVK_ANSI_I}, {L"j", kVK_ANSI_J}, {L"k", kVK_ANSI_K}, {L"l", kVK_ANSI_L}, {L"m", kVK_ANSI_M}, {L"n", kVK_ANSI_N},
    {L"o", kVK_ANSI_O}, {L"p", kVK_ANSI_P}, {L"q", kVK_ANSI_Q}, {L"r", kVK_ANSI_R}, {L"s", kVK_ANSI_S}, {L"t", kVK_ANSI_T}, {L"u", kVK_ANSI_U},
    {L"v", kVK_ANSI_V}, {L"w", kVK_ANSI_W}, {L"x", kVK_ANSI_X}, {L"y", kVK_ANSI_Y}, {L"z", kVK_ANSI_Z}, {L"0", kVK_ANSI_0}, {L"1", kVK_ANSI_1},
    {L"2", kVK_ANSI_2}, {L"3", kVK_ANSI_3}, {L"4", kVK_ANSI_4}, {L"5", kVK_ANSI_5}, {L"6", kVK_ANSI_6}, {L"7", kVK_ANSI_7}, {L"8", kVK_ANSI_8},
    {L"9", kVK_ANSI_9}, {L"f1", kVK_F1}, {L"f2", kVK_F2}, {L"f3", kVK_F3}, {L"f4", kVK_F4}, {L"f5", kVK_F5}, {L"f6", kVK_F6},
    {L"f7", kVK_F7}, {L"f8", kVK_F8}, {L"f9", kVK_F9}, {L"f10", kVK_F10}, {L"f11", kVK_F11}, {L"f12", kVK_F12}, {L"f13", kVK_F13},
    {L"f14", kVK_F14}, {L"f15", kVK_F15}, {L"f16", kVK_F16}, {L"f17", kVK_F17}, {L"f18", kVK_F18}, {L"f19", kVK_F19}, {L"f20", kVK_F20},
    {L"space", kVK_Space},
};

std::wstring Trim(std::wstring_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t')) {
        --end;
    }
    return std::wstring(text.substr(begin, end - begin));
}

bool ParseHotkey(const std::wstring& text, Hotkey& hotkey) {
    hotkey = Hotkey{};
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
            hotkey.modifiers |= controlKey;
        } else if (part == L"alt" || part == L"option" || part == L"opt") {
            hotkey.modifiers |= optionKey;
        } else if (part == L"shift") {
            hotkey.modifiers |= shiftKey;
        } else if (part == L"cmd" || part == L"command" || part == L"win" || part == L"windows" || part == L"super") {
            hotkey.modifiers |= cmdKey;
        } else {
            return false;
        }
    }
    for (const KeyName& key : kKeys) {
        if (parts.back() == key.name) {
            hotkey.keyCode = key.code;
            hotkey.key = key.name;
            return hotkey.modifiers != 0;
        }
    }
    return false;
}

UInt32 LayoutKeyCode(const Hotkey& hotkey) {
    if (hotkey.key.size() != 1) {
        return hotkey.keyCode;
    }
    TISInputSourceRef source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
    if (source == nullptr) {
        return hotkey.keyCode;
    }
    UInt32 code = hotkey.keyCode;
    CFDataRef data = static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    if (data != nullptr) {
        const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
        for (UInt16 key = 0; key < 128; ++key) {
            UInt32 deadKeys = 0;
            UniChar characters[4] = {};
            UniCharCount length = 0;
            if (UCKeyTranslate(layout, key, kUCKeyActionDisplay, 0, LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit, &deadKeys, 4, &length, characters) == noErr &&
                length == 1 && static_cast<wchar_t>(std::towlower(characters[0])) == hotkey.key[0]) {
                code = key;
                break;
            }
        }
    }
    CFRelease(source);
    return code;
}

std::wstring KeyLabel(const std::wstring& key) {
    if (key == L"space") {
        return L"Space";
    }
    std::wstring label = key;
    for (wchar_t& c : label) {
        c = static_cast<wchar_t>(std::towupper(c));
    }
    return label;
}

void ApplyShortcut(NSMenuItem* item, const std::wstring& shortcut) {
    NSEventModifierFlags flags = 0;
    size_t index = 0;
    for (; index < shortcut.size(); ++index) {
        const wchar_t c = shortcut[index];
        if (c == L'⌃') {
            flags |= NSEventModifierFlagControl;
        } else if (c == L'⌥') {
            flags |= NSEventModifierFlagOption;
        } else if (c == L'⇧') {
            flags |= NSEventModifierFlagShift;
        } else if (c == L'⌘') {
            flags |= NSEventModifierFlagCommand;
        } else {
            break;
        }
    }
    const std::wstring key = ToLower(shortcut.substr(index));
    NSString* equivalent = nil;
    if (key.size() == 1) {
        equivalent = ToNSString(key);
    } else if (key == L"space") {
        equivalent = @" ";
    } else if (key.size() >= 2 && key[0] == L'f') {
        const int number = std::wcstol(key.c_str() + 1, nullptr, 10);
        if (number >= 1 && number <= 20) {
            const unichar function = static_cast<unichar>(NSF1FunctionKey + number - 1);
            equivalent = [NSString stringWithCharacters:&function length:1];
        }
    }
    if (equivalent != nil) {
        item.keyEquivalent = equivalent;
        item.keyEquivalentModifierMask = flags;
    }
}

NSAppearance* MenuAppearance() {
    return [NSAppearance appearanceNamed:g_darkMenus ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
}

void ActivateSelf() {
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
        [NSApp activateIgnoringOtherApps:YES];
    }
}

void RestoreFrontmost(NSRunningApplication* previous) {
    if (previous != nil && ![previous isEqual:[NSRunningApplication currentApplication]] && !previous.terminated) {
        [previous activateWithOptions:0];
    }
}

NSImage* TrayImage() {
    NSImage* image = [NSImage imageWithSize:NSMakeSize(18, 18)
                                    flipped:YES
                             drawingHandler:^BOOL(NSRect rect) {
                                 const CGFloat size = rect.size.width;
                                 auto oval = [&](CGFloat cx, CGFloat cy, CGFloat rx, CGFloat ry) {
                                     return [NSBezierPath bezierPathWithOvalInRect:NSMakeRect((cx - rx) * size, (cy - ry) * size, rx * 2 * size, ry * 2 * size)];
                                 };
                                 NSBezierPath* body = oval(0.5, 0.53, 0.42, 0.37);
                                 [body appendBezierPath:oval(0.385, 0.5, 0.06, 0.09)];
                                 [body appendBezierPath:oval(0.615, 0.5, 0.06, 0.09)];
                                 body.windingRule = NSWindingRuleEvenOdd;
                                 [[NSColor blackColor] setFill];
                                 [body fill];
                                 [oval(0.33, 0.9, 0.1, 0.07) fill];
                                 [oval(0.67, 0.9, 0.1, 0.07) fill];
                                 return YES;
                             }];
    [image setTemplate:YES];
    return image;
}

}

}

@interface CPMenuTarget : NSObject
@property(nonatomic) uint32_t selected;
- (void)choose:(NSMenuItem*)item;
@end

@implementation CPMenuTarget
- (void)choose:(NSMenuItem*)item {
    self.selected = static_cast<uint32_t>(item.tag);
}
@end

@interface CPPlatformBridge : NSObject <NSApplicationDelegate>
@property(nonatomic, assign) cp::PlatformEvents* events;
- (void)trayClicked:(id)sender;
@end

@implementation CPPlatformBridge

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)flag {
    if (self.events != nullptr) {
        self.events->OnShowRequested();
    }
    return NO;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    if (self.events != nullptr) {
        self.events->OnCloseRequested();
    }
    return NSTerminateNow;
}

- (void)trayClicked:(id)sender {
    if (self.events == nullptr) {
        return;
    }
    NSEvent* event = NSApp.currentEvent;
    const bool menu = event.type == NSEventTypeRightMouseUp || event.type == NSEventTypeRightMouseDown || (event.modifierFlags & NSEventModifierFlagControl) != 0;
    if (menu) {
        self.events->OnTrayMenu();
    } else {
        self.events->OnTrayClicked();
    }
}

- (void)observeValueForKeyPath:(NSString*)keyPath ofObject:(id)object change:(NSDictionary*)change context:(void*)context {
    if (self.events != nullptr) {
        self.events->OnThemeChanged();
    }
}

@end

namespace cp {

namespace {

NSMenu* BuildMenu(const std::vector<MenuItem>& items, CPMenuTarget* target) {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;
    for (const MenuItem& item : items) {
        switch (item.kind) {
        case MenuItem::Kind::Separator:
            [menu addItem:[NSMenuItem separatorItem]];
            break;
        case MenuItem::Kind::Submenu: {
            NSMenuItem* entry = [[NSMenuItem alloc] initWithTitle:ToNSString(item.label) action:nil keyEquivalent:@""];
            entry.submenu = BuildMenu(item.children, target);
            [menu addItem:entry];
            break;
        }
        case MenuItem::Kind::Command: {
            NSMenuItem* entry = [[NSMenuItem alloc] initWithTitle:ToNSString(item.label) action:@selector(choose:) keyEquivalent:@""];
            entry.target = target;
            entry.tag = static_cast<NSInteger>(item.command);
            entry.state = item.checked ? NSControlStateValueOn : NSControlStateValueOff;
            entry.enabled = item.enabled;
            if (!item.shortcut.empty()) {
                ApplyShortcut(entry, item.shortcut);
            }
            [menu addItem:entry];
            break;
        }
        }
    }
    return menu;
}

OSStatus HotkeyHandler(EventHandlerCallRef, EventRef event, void* context) {
    EventHotKeyID identifier = {};
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, nullptr, sizeof(identifier), nullptr, &identifier) != noErr) {
        return eventNotHandledErr;
    }
    if (identifier.signature != kHotkeySignature || identifier.id != kHotkeyIdentifier) {
        return eventNotHandledErr;
    }
    static_cast<PlatformEvents*>(context)->OnHotkey();
    return noErr;
}

CFDataRef PortCallback(CFMessagePortRef, SInt32 message, CFDataRef data, void* context) {
    auto* events = static_cast<PlatformEvents*>(context);
    if (message == kPortMessageShow) {
        events->OnShowRequested();
    } else if (message == kPortMessageHook && data != nullptr) {
        const CFIndex length = CFDataGetLength(data);
        if (length > 0 && static_cast<size_t>(length) <= kPortMessageLimit) {
            events->OnHookEvent(std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<size_t>(length)));
        }
    }
    return nullptr;
}

}

struct Platform::State {
    CPPlatformBridge* bridge = nil;
    NSStatusItem* tray = nil;
    CFMessagePortRef port = nullptr;
    CFRunLoopSourceRef portSource = nullptr;
    EventHandlerRef hotkeyHandler = nullptr;
    EventHotKeyRef hotkey = nullptr;
    std::wstring hotkeyText;
    id inputSourceObserver = nil;
    std::unordered_map<uint32_t, NSTimer*> timers;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    bool observingTheme = false;
};

Platform::Platform(PlatformEvents& events) : events_(events), state_(std::make_unique<State>()) {}

Platform::~Platform() {
    State& state = *state_;
    state.alive->store(false);
    for (auto& [id, timer] : state.timers) {
        [timer invalidate];
    }
    state.timers.clear();
    HideTray();
    UnregisterHotkey();
    if (state.inputSourceObserver != nil) {
        [[NSDistributedNotificationCenter defaultCenter] removeObserver:state.inputSourceObserver];
        state.inputSourceObserver = nil;
    }
    if (state.hotkeyHandler != nullptr) {
        RemoveEventHandler(state.hotkeyHandler);
        state.hotkeyHandler = nullptr;
    }
    if (state.portSource != nullptr) {
        CFRunLoopRemoveSource(CFRunLoopGetMain(), state.portSource, kCFRunLoopCommonModes);
        CFRelease(state.portSource);
        state.portSource = nullptr;
    }
    if (state.port != nullptr) {
        CFMessagePortInvalidate(state.port);
        CFRelease(state.port);
        state.port = nullptr;
    }
    if (state.observingTheme) {
        [NSApp removeObserver:state.bridge forKeyPath:@"effectiveAppearance"];
        state.observingTheme = false;
    }
    if (NSApp.delegate == state.bridge) {
        NSApp.delegate = nil;
    }
    state.bridge.events = nullptr;
}

bool Platform::Attach(OverlayWindow&) {
    State& state = *state_;
    CFStringRef name = CFStringCreateWithCString(nullptr, kInstancePortName, kCFStringEncodingUTF8);
    CFMessagePortContext context = {0, &events_, nullptr, nullptr, nullptr};
    Boolean shouldFree = false;
    state.port = CFMessagePortCreateLocal(nullptr, name, PortCallback, &context, &shouldFree);
    CFRelease(name);
    if (state.port == nullptr) {
        return false;
    }
    state.portSource = CFMessagePortCreateRunLoopSource(nullptr, state.port, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), state.portSource, kCFRunLoopCommonModes);
    state.bridge = [[CPPlatformBridge alloc] init];
    state.bridge.events = &events_;
    NSApp.delegate = state.bridge;
    [NSApp addObserver:state.bridge forKeyPath:@"effectiveAppearance" options:0 context:nullptr];
    state.observingTheme = true;
    const EventTypeSpec spec = {kEventClassKeyboard, kEventHotKeyPressed};
    InstallApplicationEventHandler(NewEventHandlerUPP(HotkeyHandler), 1, &spec, &events_, &state.hotkeyHandler);
    state.inputSourceObserver = [[NSDistributedNotificationCenter defaultCenter] addObserverForName:static_cast<NSString*>(kTISNotifySelectedKeyboardInputSourceChanged)
                                                                                             object:nil
                                                                                              queue:[NSOperationQueue mainQueue]
                                                                                         usingBlock:^(NSNotification*) {
                                                                                             if (!state_->hotkeyText.empty()) {
                                                                                                 const std::wstring text = state_->hotkeyText;
                                                                                                 RegisterHotkey(text);
                                                                                             }
                                                                                         }];
    StartClaudeForegroundTracking();
    return true;
}

void Platform::Run() {
    [NSApp run];
}

void Platform::Quit() {
    state_->alive->store(false);
    [NSApp stop:nil];
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
    [NSApp postEvent:wake atStart:YES];
}

void Platform::PostSessionsChanged() {
    std::shared_ptr<std::atomic<bool>> alive = state_->alive;
    PlatformEvents* events = &events_;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (alive->load()) {
            events->OnSessionsChanged();
        }
    });
}

void Platform::SetTimer(uint32_t id, uint32_t delayMs) {
    KillTimer(id);
    PlatformEvents* events = &events_;
    NSTimer* timer = [NSTimer timerWithTimeInterval:std::max<uint32_t>(delayMs, 1) / 1000.0
                                            repeats:YES
                                              block:^(NSTimer*) {
                                                  events->OnTimer(id);
                                              }];
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    state_->timers[id] = timer;
}

void Platform::KillTimer(uint32_t id) {
    const auto found = state_->timers.find(id);
    if (found == state_->timers.end()) {
        return;
    }
    [found->second invalidate];
    state_->timers.erase(found);
}

bool Platform::RegisterHotkey(const std::wstring& text) {
    Hotkey hotkey;
    if (!ParseHotkey(text, hotkey)) {
        return false;
    }
    UnregisterHotkey();
    const EventHotKeyID identifier = {kHotkeySignature, kHotkeyIdentifier};
    if (RegisterEventHotKey(LayoutKeyCode(hotkey), hotkey.modifiers, identifier, GetApplicationEventTarget(), 0, &state_->hotkey) != noErr) {
        state_->hotkey = nullptr;
        return false;
    }
    state_->hotkeyText = text;
    return true;
}

void Platform::UnregisterHotkey() {
    state_->hotkeyText.clear();
    if (state_->hotkey != nullptr) {
        UnregisterEventHotKey(state_->hotkey);
        state_->hotkey = nullptr;
    }
}

void Platform::ShowTray(const std::wstring& tooltip) {
    State& state = *state_;
    if (state.tray == nil) {
        state.tray = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
        NSStatusBarButton* button = state.tray.button;
        button.image = TrayImage();
        button.target = state.bridge;
        button.action = @selector(trayClicked:);
        [button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
    }
    state.tray.button.toolTip = ToNSString(tooltip);
}

void Platform::HideTray() {
    State& state = *state_;
    if (state.tray != nil) {
        [[NSStatusBar systemStatusBar] removeStatusItem:state.tray];
        state.tray = nil;
    }
}

uint32_t Platform::ShowMenu(const std::vector<MenuItem>& items, PointI screen) {
    CPMenuTarget* target = [[CPMenuTarget alloc] init];
    NSMenu* menu = BuildMenu(items, target);
    menu.appearance = MenuAppearance();
    const NSPoint location = NSMakePoint(screen.x, PrimaryScreenHeight() - screen.y);
    [menu popUpMenuPositioningItem:nil atLocation:location inView:nil];
    return target.selected;
}

void Platform::Alert(const std::wstring& text, const std::wstring& title, AlertKind kind) {
    NSRunningApplication* previous = [NSWorkspace sharedWorkspace].frontmostApplication;
    ActivateSelf();
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = ToNSString(title);
    alert.informativeText = ToNSString(text);
    alert.alertStyle = kind == AlertKind::Error ? NSAlertStyleCritical : NSAlertStyleWarning;
    alert.window.appearance = MenuAppearance();
    [alert runModal];
    RestoreFrontmost(previous);
}

bool Platform::Confirm(const std::wstring& text, const std::wstring& title) {
    NSRunningApplication* previous = [NSWorkspace sharedWorkspace].frontmostApplication;
    ActivateSelf();
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = ToNSString(title);
    alert.informativeText = ToNSString(text);
    alert.alertStyle = NSAlertStyleInformational;
    alert.window.appearance = MenuAppearance();
    [alert addButtonWithTitle:ToNSString(Text().buttonYes)];
    [alert addButtonWithTitle:ToNSString(Text().buttonNo)];
    const bool accepted = [alert runModal] == NSAlertFirstButtonReturn;
    RestoreFrontmost(previous);
    return accepted;
}

bool Platform::OpenFolder(const std::wstring& path) {
    return [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:ToNSString(path) isDirectory:YES]];
}

bool SystemUsesDarkTheme() {
    NSAppearance* appearance = NSApp != nil ? NSApp.effectiveAppearance : [NSAppearance currentDrawingAppearance];
    return [[appearance bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]] isEqualToString:NSAppearanceNameDarkAqua];
}

void ApplyMenuTheme(bool dark) {
    g_darkMenus = dark;
}

std::wstring HotkeyLabel(const std::wstring& text) {
    Hotkey hotkey;
    if (!ParseHotkey(text, hotkey)) {
        return {};
    }
    std::wstring label;
    if ((hotkey.modifiers & controlKey) != 0) {
        label += L"⌃";
    }
    if ((hotkey.modifiers & optionKey) != 0) {
        label += L"⌥";
    }
    if ((hotkey.modifiers & shiftKey) != 0) {
        label += L"⇧";
    }
    if ((hotkey.modifiers & cmdKey) != 0) {
        label += L"⌘";
    }
    return label + KeyLabel(hotkey.key);
}

bool IsAutostartEnabled(const std::wstring&) {
    return SMAppService.mainAppService.status == SMAppServiceStatusEnabled;
}

bool SetAutostart(const std::wstring&, bool enabled) {
    SMAppService* service = SMAppService.mainAppService;
    NSError* error = nil;
    if (enabled) {
        [service registerAndReturnError:&error];
        if (service.status == SMAppServiceStatusRequiresApproval) {
            [SMAppService openSystemSettingsLoginItems];
        }
        return service.status == SMAppServiceStatusEnabled;
    }
    [service unregisterAndReturnError:&error];
    return service.status != SMAppServiceStatusEnabled;
}

bool RunsFromTemporaryLocation(const std::wstring& executable) {
    return executable.find(L"/AppTranslocation/") != std::wstring::npos;
}

Language SystemLanguage() {
    NSString* preferred = [NSLocale preferredLanguages].firstObject;
    return preferred != nil && [preferred.lowercaseString hasPrefix:@"ru"] ? Language::Russian : Language::English;
}

}
