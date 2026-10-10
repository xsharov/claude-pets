#include "overlay_window.h"

#include "mac/mac_util.h"
#include "overlay_metrics.h"
#include "renderer.h"
#include "util.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <cmath>
#include <unordered_map>

namespace cp {

namespace {

constexpr double kWheelStep = 24.0;

NSScreen* ScreenWithId(DisplayId id) {
    for (NSScreen* screen in [NSScreen screens]) {
        NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
        if (number != nil && static_cast<DisplayId>(number.unsignedIntValue) == id) {
            return screen;
        }
    }
    return nil;
}

DisplayId IdOf(NSScreen* screen) {
    NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
    return number != nil ? static_cast<DisplayId>(number.unsignedIntValue) : 0;
}

DisplayInfo Describe(NSScreen* screen) {
    DisplayInfo display;
    if (screen == nil) {
        return display;
    }
    display.id = IdOf(screen);
    display.work = FromCocoaRect(screen.visibleFrame);
    display.scale = 1.0f;
    return display;
}

double DistanceSquared(const RectI& rect, PointI point) {
    const double dx = point.x < rect.left ? rect.left - point.x : (point.x >= rect.right ? point.x - rect.right + 1 : 0);
    const double dy = point.y < rect.top ? rect.top - point.y : (point.y >= rect.bottom ? point.y - rect.bottom + 1 : 0);
    return dx * dx + dy * dy;
}

NSScreen* ScreenNearest(PointI point) {
    NSScreen* best = nil;
    double bestDistance = 0.0;
    for (NSScreen* screen in [NSScreen screens]) {
        const double distance = DistanceSquared(FromCocoaRect(screen.frame), point);
        if (best == nil || distance < bestDistance) {
            best = screen;
            bestDistance = distance;
        }
    }
    return best;
}

NSScreen* ScreenForBounds(const RectI& bounds) {
    return ScreenNearest(PointI{bounds.left + bounds.Width() / 2, bounds.top + bounds.Height() / 2});
}

NSCursor* CursorFor(CursorShape shape) {
    switch (shape) {
    case CursorShape::Hand:
        return [NSCursor pointingHandCursor];
    case CursorShape::Move:
        return [NSCursor closedHandCursor];
    case CursorShape::Arrow:
        break;
    }
    return [NSCursor arrowCursor];
}

}

DisplayInfo DisplayFromPoint(PointI point) {
    return Describe(ScreenNearest(point));
}

DisplayInfo PrimaryDisplay() {
    NSArray<NSScreen*>* screens = [NSScreen screens];
    return Describe(screens.count > 0 ? screens[0] : nil);
}

std::optional<DisplayInfo> FindDisplay(const std::wstring& key) {
    for (NSScreen* screen in [NSScreen screens]) {
        if (DisplayKey(IdOf(screen)) == key) {
            return Describe(screen);
        }
    }
    return std::nullopt;
}

std::wstring DisplayKey(DisplayId display) {
    NSScreen* screen = ScreenWithId(display);
    if (screen == nil) {
        return {};
    }
    const RectI frame = FromCocoaRect(screen.frame);
    std::wstring name = FromNSString(screen.localizedName);
    if (name.empty()) {
        name = L"display" + std::to_wstring(display);
    }
    return name + L"@" + std::to_wstring(frame.left) + L"," + std::to_wstring(frame.top) + L"," + std::to_wstring(frame.right) + L"," + std::to_wstring(frame.bottom);
}

PointI CursorPosition() {
    const NSPoint location = [NSEvent mouseLocation];
    return PointI{static_cast<int32_t>(std::floor(location.x)), static_cast<int32_t>(std::floor(PrimaryScreenHeight() - location.y))};
}

bool PrimaryButtonDown() {
    return ([NSEvent pressedMouseButtons] & 1u) != 0;
}

bool AltKeyDown() {
    return ([NSEvent modifierFlags] & NSEventModifierFlagOption) != 0;
}

bool ReducedMotionEnabled() {
    return [NSWorkspace sharedWorkspace].accessibilityDisplayShouldReduceMotion;
}

}

namespace cp {
struct OverlayWindowBridge;
}

@interface CPOverlayPanel : NSPanel
@end

@implementation CPOverlayPanel
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

@interface CPOverlayView : NSView
@property(nonatomic, assign) cp::OverlayWindowBridge* bridge;
@end

@interface CPOverlayTicker : NSObject
@property(nonatomic, assign) cp::OverlayWindowBridge* bridge;
- (void)frame:(id)sender;
@end

namespace cp {

struct OverlayWindowBridge {
    void Pointer(PointI screen);
    void PrimaryDown(NSEvent* event);
    void PrimaryDragged(NSEvent* event);
    void PrimaryUp(NSEvent* event);
    void SecondaryUp(NSEvent* event);
    void Wheel(NSEvent* event);
    void Frame();
    void Timer(uint32_t id);
    void RefreshInteractivity();
    PointI ClientOf(NSEvent* event) const;
    PointI ClientOf(PointI screen) const;
    void SetIgnoring(bool ignoring);

    OverlayWindowEvents* events = nullptr;
    Renderer* renderer = nullptr;
    CPOverlayPanel* panel = nil;
    CPOverlayView* view = nil;
    CPOverlayTicker* ticker = nil;
    id displayLink = nil;
    id globalMonitor = nil;
    id localMonitor = nil;
    NSMutableArray* observers = nil;
    std::unordered_map<uint32_t, NSTimer*> timers;
    RectI bounds = {};
    double backing = 0.0;
    bool hasFrame = false;
    bool ignoring = true;
    bool hovering = false;
    bool pressed = false;
    bool secondaryClick = false;
    bool frameClockActive = false;
    double wheelAccumulator = 0.0;
};

PointI OverlayWindowBridge::ClientOf(PointI screen) const {
    return PointI{screen.x - bounds.left, screen.y - bounds.top};
}

PointI OverlayWindowBridge::ClientOf(NSEvent* event) const {
    const NSPoint local = [view convertPoint:event.locationInWindow fromView:nil];
    return PointI{static_cast<int32_t>(std::floor(local.x)), static_cast<int32_t>(std::floor(local.y))};
}

void OverlayWindowBridge::SetIgnoring(bool value) {
    if (ignoring == value) {
        return;
    }
    ignoring = value;
    panel.ignoresMouseEvents = value;
}

void OverlayWindowBridge::Pointer(PointI screen) {
    if (pressed || secondaryClick || events == nullptr || panel == nil || !panel.visible) {
        return;
    }
    const PointI client = ClientOf(screen);
    const bool inside = client.x >= 0 && client.y >= 0 && client.x < bounds.Width() && client.y < bounds.Height();
    if (inside && events->Interactive(client)) {
        SetIgnoring(false);
        hovering = true;
        events->OnPointerMove(client, false);
        [CursorFor(events->CursorAt(client)) set];
        return;
    }
    SetIgnoring(true);
    if (hovering) {
        hovering = false;
        events->OnPointerLeave();
    }
}

void OverlayWindowBridge::RefreshInteractivity() {
    if (pressed || secondaryClick || events == nullptr || panel == nil) {
        return;
    }
    const PointI client = ClientOf(CursorPosition());
    const bool inside = client.x >= 0 && client.y >= 0 && client.x < bounds.Width() && client.y < bounds.Height();
    SetIgnoring(!(inside && events->Interactive(client)));
}

void OverlayWindowBridge::PrimaryDown(NSEvent* event) {
    if ((event.modifierFlags & NSEventModifierFlagControl) != 0) {
        secondaryClick = true;
        return;
    }
    pressed = true;
    events->OnPrimaryDown(ClientOf(event));
}

void OverlayWindowBridge::PrimaryDragged(NSEvent* event) {
    if (!pressed) {
        return;
    }
    const PointI client = ClientOf(event);
    events->OnPointerMove(client, true);
    [CursorFor(events->CursorAt(client)) set];
}

void OverlayWindowBridge::PrimaryUp(NSEvent* event) {
    if (secondaryClick) {
        secondaryClick = false;
        events->OnSecondaryUp(ClientOf(event));
        Pointer(CursorPosition());
        return;
    }
    if (!pressed) {
        return;
    }
    pressed = false;
    events->OnPrimaryUp(ClientOf(event));
    Pointer(CursorPosition());
}

void OverlayWindowBridge::SecondaryUp(NSEvent* event) {
    events->OnSecondaryUp(ClientOf(event));
    Pointer(CursorPosition());
}

void OverlayWindowBridge::Wheel(NSEvent* event) {
    if (event.momentumPhase != NSEventPhaseNone) {
        return;
    }
    if (event.phase == NSEventPhaseBegan || event.phase == NSEventPhaseMayBegin) {
        wheelAccumulator = 0.0;
    }
    const double delta = event.scrollingDeltaY;
    if (delta == 0.0) {
        return;
    }
    if (!event.hasPreciseScrollingDeltas) {
        events->OnWheel(delta > 0.0 ? 120 : -120);
        return;
    }
    wheelAccumulator += delta;
    while (std::fabs(wheelAccumulator) >= kWheelStep) {
        const int direction = wheelAccumulator > 0.0 ? 1 : -1;
        wheelAccumulator -= direction * kWheelStep;
        events->OnWheel(direction * 120);
    }
}

void OverlayWindowBridge::Frame() {
    if (frameClockActive && events != nullptr) {
        events->OnFrame();
    }
}

void OverlayWindowBridge::Timer(uint32_t id) {
    if (events != nullptr) {
        events->OnTimer(id);
    }
}

}

@implementation CPOverlayView

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}

- (BOOL)mouseDownCanMoveWindow {
    return NO;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* area in self.trackingAreas) {
        [self removeTrackingArea:area];
    }
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                       options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                         owner:self
                                                      userInfo:nil]];
}

- (void)mouseMoved:(NSEvent*)event {
    if (self.bridge != nullptr) {
        self.bridge->Pointer(cp::CursorPosition());
    }
}

- (void)mouseEntered:(NSEvent*)event {
    if (self.bridge != nullptr) {
        self.bridge->Pointer(cp::CursorPosition());
    }
}

- (void)mouseExited:(NSEvent*)event {
    if (self.bridge != nullptr) {
        self.bridge->Pointer(cp::CursorPosition());
    }
}

- (void)mouseDown:(NSEvent*)event {
    if (self.bridge != nullptr) {
        self.bridge->PrimaryDown(event);
    }
}

- (void)mouseDragged:(NSEvent*)event {
    if (self.bridge != nullptr) {
        self.bridge->PrimaryDragged(event);
    }
}

- (void)mouseUp:(NSEvent*)event {
    if (self.bridge != nullptr) {
        self.bridge->PrimaryUp(event);
    }
}

- (void)rightMouseDown:(NSEvent*)event {
}

- (void)rightMouseUp:(NSEvent*)event {
    if (self.bridge != nullptr) {
        self.bridge->SecondaryUp(event);
    }
}

- (void)scrollWheel:(NSEvent*)event {
    if (self.bridge != nullptr) {
        self.bridge->Wheel(event);
    }
}

@end

@implementation CPOverlayTicker
- (void)frame:(id)sender {
    if (self.bridge != nullptr) {
        self.bridge->Frame();
    }
}
@end

namespace cp {

struct OverlayWindow::Impl : OverlayWindowBridge {};

OverlayWindow::OverlayWindow() : impl_(std::make_unique<Impl>()) {}

OverlayWindow::~OverlayWindow() {
    Destroy();
}

bool OverlayWindow::Create(OverlayWindowEvents& events, Renderer& renderer) {
    Impl& impl = *impl_;
    impl.events = &events;
    impl.renderer = &renderer;
    impl.panel = [[CPOverlayPanel alloc] initWithContentRect:NSMakeRect(0, 0, 1, 1)
                                                   styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    if (impl.panel == nil) {
        return false;
    }
    CPOverlayPanel* panel = impl.panel;
    panel.releasedWhenClosed = NO;
    panel.opaque = NO;
    panel.backgroundColor = [NSColor clearColor];
    panel.hasShadow = NO;
    panel.floatingPanel = YES;
    panel.level = NSStatusWindowLevel;
    panel.hidesOnDeactivate = NO;
    panel.becomesKeyOnlyIfNeeded = YES;
    panel.ignoresMouseEvents = YES;
    panel.acceptsMouseMovedEvents = YES;
    panel.animationBehavior = NSWindowAnimationBehaviorNone;
    panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle |
                               NSWindowCollectionBehaviorFullScreenAuxiliary;
    panel.title = @"Claude Pets";
    panel.colorSpace = [NSColorSpace sRGBColorSpace];
    CPOverlayView* view = [[CPOverlayView alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
    view.bridge = &impl;
    CALayer* layer = [CALayer layer];
    layer.opaque = NO;
    layer.geometryFlipped = YES;
    view.layer = layer;
    view.wantsLayer = YES;
    renderer.AttachLayer((__bridge void*)layer);
    panel.contentView = view;
    impl.view = view;
    impl.ignoring = true;

    Impl* raw = &impl;
    impl.globalMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged
                                                                handler:^(NSEvent*) {
                                                                    raw->Pointer(CursorPosition());
                                                                }];
    impl.observers = [NSMutableArray array];
    [impl.observers addObject:[[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidChangeScreenParametersNotification
                                                                                object:nil
                                                                                 queue:[NSOperationQueue mainQueue]
                                                                            usingBlock:^(NSNotification*) {
                                                                                if (raw->events != nullptr) {
                                                                                    raw->events->OnDisplayChanged();
                                                                                }
                                                                            }]];
    [impl.observers addObject:[[NSWorkspace sharedWorkspace].notificationCenter addObserverForName:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
                                                                                             object:nil
                                                                                              queue:[NSOperationQueue mainQueue]
                                                                                         usingBlock:^(NSNotification*) {
                                                                                             if (raw->events != nullptr) {
                                                                                                 raw->events->OnReducedMotionChanged();
                                                                                             }
                                                                                         }]];
    impl.ticker = [[CPOverlayTicker alloc] init];
    impl.ticker.bridge = &impl;
    if (@available(macOS 14.0, *)) {
        CADisplayLink* link = [view displayLinkWithTarget:impl.ticker selector:@selector(frame:)];
        link.paused = YES;
        [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
        impl.displayLink = link;
    }
    return true;
}

void OverlayWindow::Destroy() {
    Impl& impl = *impl_;
    if (impl.panel == nil) {
        return;
    }
    for (auto& [id, timer] : impl.timers) {
        [timer invalidate];
    }
    impl.timers.clear();
    if (@available(macOS 14.0, *)) {
        [static_cast<CADisplayLink*>(impl.displayLink) invalidate];
    }
    impl.displayLink = nil;
    impl.ticker.bridge = nullptr;
    impl.ticker = nil;
    if (impl.globalMonitor != nil) {
        [NSEvent removeMonitor:impl.globalMonitor];
        impl.globalMonitor = nil;
    }
    for (id observer in impl.observers) {
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        [[NSWorkspace sharedWorkspace].notificationCenter removeObserver:observer];
    }
    impl.observers = nil;
    impl.view.bridge = nullptr;
    [impl.panel orderOut:nil];
    [impl.panel close];
    impl.view = nil;
    impl.panel = nil;
    impl.frameClockActive = false;
}

bool OverlayWindow::Created() const {
    return impl_->panel != nil;
}

void OverlayWindow::Show() {
    [impl_->panel orderFrontRegardless];
    impl_->RefreshInteractivity();
}

void OverlayWindow::Hide() {
    Impl& impl = *impl_;
    [impl.panel orderOut:nil];
    impl.hovering = false;
    impl.pressed = false;
    impl.secondaryClick = false;
    impl.SetIgnoring(true);
}

void OverlayWindow::EnsureTopmost() {
    if (impl_->panel.visible) {
        [impl_->panel orderFrontRegardless];
    }
}

void OverlayWindow::Move(PointI origin) {
    Impl& impl = *impl_;
    if (impl.panel == nil) {
        return;
    }
    const int32_t width = impl.bounds.Width();
    const int32_t height = impl.bounds.Height();
    impl.bounds = RectI{origin.x, origin.y, origin.x + width, origin.y + height};
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [impl.panel setFrameOrigin:ToCocoaRect(impl.bounds).origin];
    [CATransaction commit];
    impl.RefreshInteractivity();
}

bool OverlayWindow::Present(const RectI& bounds, float scale, const RectI*) {
    Impl& impl = *impl_;
    if (impl.panel == nil || bounds.Width() <= 0 || bounds.Height() <= 0) {
        return false;
    }
    NSScreen* screen = ScreenForBounds(bounds);
    const double backing = screen != nil ? screen.backingScaleFactor : 2.0;
    const int widthPx = static_cast<int>(std::lround(bounds.Width() * backing));
    const int heightPx = static_cast<int>(std::lround(bounds.Height() * backing));
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (!impl.hasFrame || !(bounds == impl.bounds)) {
        [impl.panel setFrame:ToCocoaRect(bounds) display:NO];
    }
    bool drawn = impl.renderer->BeginFrame(widthPx, heightPx, static_cast<float>(scale * backing), nullptr);
    if (drawn) {
        impl.events->Paint();
        drawn = impl.renderer->EndFrame();
    }
    [CATransaction commit];
    impl.bounds = bounds;
    impl.backing = backing;
    impl.hasFrame = true;
    impl.RefreshInteractivity();
    return drawn;
}

void OverlayWindow::SetTimer(uint32_t id, uint32_t delayMs) {
    Impl& impl = *impl_;
    KillTimer(id);
    Impl* raw = &impl;
    NSTimer* timer = [NSTimer timerWithTimeInterval:std::max<uint32_t>(delayMs, 1) / 1000.0
                                            repeats:YES
                                              block:^(NSTimer*) {
                                                  raw->Timer(id);
                                              }];
    timer.tolerance = std::min(0.004, delayMs / 10000.0);
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
    impl.timers[id] = timer;
}

void OverlayWindow::KillTimer(uint32_t id) {
    Impl& impl = *impl_;
    const auto found = impl.timers.find(id);
    if (found == impl.timers.end()) {
        return;
    }
    [found->second invalidate];
    impl.timers.erase(found);
}

void OverlayWindow::TrackPointerLeave() {}

void OverlayWindow::Capture() {}

void OverlayWindow::ReleaseCapture() {}

PointI OverlayWindow::ScreenToClient(PointI point) const {
    return impl_->ClientOf(point);
}

PointI OverlayWindow::ClientToScreen(PointI point) const {
    return PointI{point.x + impl_->bounds.left, point.y + impl_->bounds.top};
}

void OverlayWindow::SetFrameClockActive(bool active) {
    Impl& impl = *impl_;
    impl.frameClockActive = active;
    if (@available(macOS 14.0, *)) {
        static_cast<CADisplayLink*>(impl.displayLink).paused = !active;
    }
}

bool OverlayWindow::FrameClockActive() const {
    return impl_->frameClockActive;
}

bool OverlayWindow::FrameClockRunning() const {
    return impl_->displayLink != nil;
}

void OverlayWindow::SetFrameClockDisplay(DisplayId) {}

void* OverlayWindow::NativeWindow() const {
    return (__bridge void*)impl_->panel;
}

}
