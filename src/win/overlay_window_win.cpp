#include "overlay_window.h"

#include "overlay_metrics.h"
#include "renderer.h"
#include "resource.h"
#include "win/frame_clock.h"
#include "win/win_util.h"

#include <shellscalingapi.h>
#include <windowsx.h>

namespace cp {

namespace {

constexpr UINT kFrameMessage = WM_APP + 32;

float MonitorScale(HMONITOR monitor) {
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) {
        dpiX = 96;
    }
    return static_cast<float>(dpiX) / 96.0f;
}

DisplayInfo Describe(HMONITOR monitor) {
    DisplayInfo display;
    display.id = reinterpret_cast<DisplayId>(monitor);
    display.scale = MonitorScale(monitor);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        display.work = RectI{info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom};
        display.movable = display.work;
    }
    return display;
}

struct MonitorSearch {
    std::wstring key;
    HMONITOR found = nullptr;
};

BOOL CALLBACK FindMonitorByKey(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* search = reinterpret_cast<MonitorSearch*>(parameter);
    if (DisplayKey(reinterpret_cast<DisplayId>(monitor)) == search->key) {
        search->found = monitor;
        return FALSE;
    }
    return TRUE;
}

}

DisplayInfo DisplayFromPoint(PointI point) {
    return Describe(MonitorFromPoint(POINT{point.x, point.y}, MONITOR_DEFAULTTONEAREST));
}

DisplayInfo PrimaryDisplay() {
    return Describe(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
}

std::optional<DisplayInfo> FindDisplay(const std::wstring& key) {
    MonitorSearch search;
    search.key = key;
    EnumDisplayMonitors(nullptr, nullptr, FindMonitorByKey, reinterpret_cast<LPARAM>(&search));
    if (search.found == nullptr) {
        return std::nullopt;
    }
    return Describe(search.found);
}

std::wstring DisplayKey(DisplayId display) {
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(reinterpret_cast<HMONITOR>(display), &info)) {
        return {};
    }
    const RECT& r = info.rcMonitor;
    return std::wstring(info.szDevice) + L"@" + std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," + std::to_wstring(r.right) + L"," +
           std::to_wstring(r.bottom);
}

PointI CursorPosition() {
    POINT cursor = {};
    GetCursorPos(&cursor);
    return PointI{cursor.x, cursor.y};
}

bool PrimaryButtonDown() {
    const int primary = GetSystemMetrics(SM_SWAPBUTTON) != 0 ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(primary) & 0x8000) != 0;
}

bool AltKeyDown() {
    return GetKeyState(VK_MENU) < 0;
}

bool ReducedMotionEnabled() {
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    return animations == FALSE;
}

struct OverlayWindow::Impl {
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    HWND window = nullptr;
    OverlayWindowEvents* events = nullptr;
    Renderer* renderer = nullptr;
    FrameClock frameClock;
    bool trackingLeave = false;
    std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)> hook;
};

LRESULT CALLBACK OverlayWindow::Impl::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Impl* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Impl*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window = window;
    } else {
        self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (self == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT OverlayWindow::Impl::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (hook) {
        LRESULT result = 0;
        if (hook(message, wParam, lParam, result)) {
            return result;
        }
    }
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        events->OnPointerMove(PointI{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, (wParam & MK_LBUTTON) != 0);
        return 0;
    case WM_MOUSELEAVE:
        trackingLeave = false;
        events->OnPointerLeave();
        return 0;
    case WM_LBUTTONDOWN:
        events->OnPrimaryDown(PointI{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_LBUTTONUP:
        events->OnPrimaryUp(PointI{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_RBUTTONUP:
        events->OnSecondaryUp(PointI{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_MOUSEWHEEL:
        events->OnWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != window) {
            events->OnCaptureLost();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT cursor = {};
            GetCursorPos(&cursor);
            ::ScreenToClient(window, &cursor);
            const CursorShape shape = events->CursorAt(PointI{cursor.x, cursor.y});
            SetCursor(LoadCursorW(nullptr, shape == CursorShape::Move ? IDC_SIZEALL : (shape == CursorShape::Hand ? IDC_HAND : IDC_ARROW)));
            return TRUE;
        }
        break;
    case kFrameMessage:
        frameClock.FrameHandled();
        events->OnFrame();
        return 0;
    case WM_TIMER:
        if (wParam == metrics::kAnimationTimer || wParam == metrics::kCollapseTimer) {
            events->OnTimer(static_cast<uint32_t>(wParam));
            return 0;
        }
        break;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        events->OnDisplayChanged();
        return 0;
    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA) {
            events->OnDisplayChanged();
        }
        if (wParam == SPI_SETCLIENTAREAANIMATION) {
            events->OnReducedMotionChanged();
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

OverlayWindow::OverlayWindow() : impl_(std::make_unique<Impl>()) {}

OverlayWindow::~OverlayWindow() {
    Destroy();
}

bool OverlayWindow::Create(OverlayWindowEvents& events, Renderer& renderer) {
    impl_->events = &events;
    impl_->renderer = &renderer;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Impl::WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kOverlayWindowClass;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    windowClass.hIconSm = windowClass.hIcon;
    RegisterClassExW(&windowClass);
    impl_->window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kOverlayWindowClass, L"Claude Pets", WS_POPUP, 0, 0, 1, 1,
                                    nullptr, nullptr, instance, impl_.get());
    if (impl_->window == nullptr) {
        return false;
    }
    ChangeWindowMessageFilterEx(impl_->window, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    impl_->frameClock.Start(impl_->window, kFrameMessage);
    return true;
}

void OverlayWindow::Destroy() {
    impl_->frameClock.Stop();
    if (impl_->window != nullptr) {
        ::KillTimer(impl_->window, metrics::kAnimationTimer);
        ::KillTimer(impl_->window, metrics::kCollapseTimer);
        DestroyWindow(impl_->window);
        impl_->window = nullptr;
    }
}

bool OverlayWindow::Created() const {
    return impl_->window != nullptr;
}

void OverlayWindow::Show() {
    ShowWindow(impl_->window, SW_SHOWNOACTIVATE);
    SetWindowPos(impl_->window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void OverlayWindow::Hide() {
    ShowWindow(impl_->window, SW_HIDE);
}

void OverlayWindow::EnsureTopmost() {
    SetWindowPos(impl_->window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void OverlayWindow::Move(PointI origin) {
    SetWindowPos(impl_->window, nullptr, origin.x, origin.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool OverlayWindow::Present(const RectI& bounds, float scale, const RectI* dirty) {
    if (impl_->window == nullptr || !impl_->renderer->BeginFrame(bounds.Width(), bounds.Height(), scale, dirty)) {
        return false;
    }
    impl_->events->Paint();
    return impl_->renderer->EndFrame(impl_->window, POINT{bounds.left, bounds.top});
}

void OverlayWindow::SetTimer(uint32_t id, uint32_t delayMs) {
    ::SetTimer(impl_->window, id, delayMs, nullptr);
}

void OverlayWindow::KillTimer(uint32_t id) {
    ::KillTimer(impl_->window, id);
}

void OverlayWindow::TrackPointerLeave() {
    if (impl_->trackingLeave) {
        return;
    }
    TRACKMOUSEEVENT track = {};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = impl_->window;
    impl_->trackingLeave = TrackMouseEvent(&track) != FALSE;
}

void OverlayWindow::Capture() {
    SetCapture(impl_->window);
}

void OverlayWindow::ReleaseCapture() {
    if (GetCapture() == impl_->window) {
        ::ReleaseCapture();
    }
}

PointI OverlayWindow::ScreenToClient(PointI point) const {
    POINT value = {point.x, point.y};
    ::ScreenToClient(impl_->window, &value);
    return PointI{value.x, value.y};
}

PointI OverlayWindow::ClientToScreen(PointI point) const {
    POINT value = {point.x, point.y};
    ::ClientToScreen(impl_->window, &value);
    return PointI{value.x, value.y};
}

void OverlayWindow::SetFrameClockActive(bool active) {
    impl_->frameClock.SetActive(active);
}

bool OverlayWindow::FrameClockActive() const {
    return impl_->frameClock.Active();
}

bool OverlayWindow::FrameClockRunning() const {
    return impl_->frameClock.Running();
}

void OverlayWindow::SetFrameClockDisplay(DisplayId display) {
    impl_->frameClock.SetMonitor(reinterpret_cast<HMONITOR>(display));
}

HWND OverlayWindow::Handle() const {
    return impl_->window;
}

void OverlayWindow::SetMessageHook(std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)> hook) {
    impl_->hook = std::move(hook);
}

}
