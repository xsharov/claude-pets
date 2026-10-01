#include "overlay.h"

#include "log.h"
#include "overlay_metrics.h"
#include "resource.h"
#include "util.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace cp {

using namespace metrics;

namespace {

constexpr wchar_t kWindowClass[] = L"ClaudePetsOverlay";

int64_t Ticks() {
    return static_cast<int64_t>(MonotonicMs());
}

bool ReducedMotion() {
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    return animations == FALSE;
}

RECT UnionRect(const RECT& a, const RECT& b) {
    if (a.right <= a.left || a.bottom <= a.top) {
        return b;
    }
    if (b.right <= b.left || b.bottom <= b.top) {
        return a;
    }
    return RECT{std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

}

Overlay::~Overlay() {
    Destroy();
}

bool Overlay::Create(HINSTANCE instance, OverlayCallbacks callbacks) {
    instance_ = instance;
    callbacks_ = std::move(callbacks);
    if (!renderer_.Initialize()) {
        return false;
    }
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &Overlay::WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    windowClass.hIconSm = windowClass.hIcon;
    RegisterClassExW(&windowClass);
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kWindowClass, L"Claude Pets", WS_POPUP, 0, 0, 1, 1,
                              nullptr, nullptr, instance, this);
    if (window_ == nullptr) {
        return false;
    }
    ChangeWindowMessageFilterEx(window_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    frameClock_.Start(window_, kFrameMessage);
    shimmerEpochMs_ = Ticks();
    lastAnimationMs_ = MonotonicMs();
    animator_.SetReducedMotion(ReducedMotion(), shimmerEpochMs_);
    animator_.Play(PetAnim::Idle, shimmerEpochMs_);
    return true;
}

void Overlay::Destroy() {
    frameClock_.Stop();
    if (window_ != nullptr) {
        KillTimer(window_, kAnimationTimer);
        KillTimer(window_, kCollapseTimer);
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

LRESULT CALLBACK Overlay::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Overlay* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Overlay*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    } else {
        self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (self == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT Overlay::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (callbacks_.messageHandler) {
        LRESULT result = 0;
        if (callbacks_.messageHandler(message, wParam, lParam, result)) {
            return result;
        }
    }
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        OnMouseMove(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, wParam);
        return 0;
    case WM_MOUSELEAVE:
        OnMouseLeave();
        return 0;
    case WM_LBUTTONDOWN:
        OnLeftDown(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_LBUTTONUP:
        OnLeftUp(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_RBUTTONUP:
        OnRightUp(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        return 0;
    case WM_MOUSEWHEEL:
        OnWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != window_) {
            OnCaptureLost();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && OnSetCursor()) {
            return TRUE;
        }
        break;
    case kFrameMessage:
        frameClock_.FrameHandled();
        Advance(MonotonicMs());
        return 0;
    case WM_TIMER:
        if (wParam == kAnimationTimer) {
            Advance(MonotonicMs());
            return 0;
        }
        if (wParam == kCollapseTimer) {
            LogLine("collapse timer hover=" + std::to_string(static_cast<int>(hover_.part)));
            KillTimer(window_, kCollapseTimer);
            collapsePending_ = false;
            if (!(hover_.part == HitPart::Pill || hover_.part == HitPart::Peek || hover_.part == HitPart::Dismiss || hover_.part == HitPart::Badge)) {
                SetExpanded(false);
            }
            return 0;
        }
        break;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        HandleDisplayChange();
        return 0;
    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA) {
            HandleDisplayChange();
        }
        if (wParam == SPI_SETCLIENTAREAANIMATION) {
            animator_.SetReducedMotion(ReducedMotion(), Ticks());
            Render(false);
            ScheduleTimer();
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

void Overlay::Show() {
    if (window_ == nullptr) {
        return;
    }
    shown_ = true;
    frameValid_ = false;
    ApplyLayout(true);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ScheduleTimer();
}

void Overlay::Hide() {
    if (window_ == nullptr) {
        return;
    }
    shown_ = false;
    KillTimer(window_, kAnimationTimer);
    frameClock_.SetActive(false);
    ShowWindow(window_, SW_HIDE);
}

void Overlay::PreserveOrderWhileHovered(std::vector<Notice>& notices) const {
    const bool trayHovered = hover_.part == HitPart::Pill || hover_.part == HitPart::Peek || hover_.part == HitPart::Dismiss || hover_.part == HitPart::Badge;
    if (!expanded_ && !trayHovered) {
        return;
    }
    std::vector<Notice> ordered;
    ordered.reserve(notices.size());
    for (const Notice& previous : notices_) {
        for (Notice& notice : notices) {
            if (!notice.id.empty() && notice.id == previous.id) {
                ordered.push_back(std::move(notice));
                notice.id.clear();
                break;
            }
        }
    }
    for (Notice& notice : notices) {
        if (!notice.id.empty()) {
            ordered.push_back(std::move(notice));
        }
    }
    notices = std::move(ordered);
}

void Overlay::SetNotices(std::vector<Notice> notices, PetAnim mascotAnim) {
    const int64_t now = Ticks();
    PreserveOrderWhileHovered(notices);
    const bool listChanged = notices != notices_;
    notices_ = std::move(notices);
    if (VisibleNoticeCount() <= 1) {
        expanded_ = false;
        scrollOffset_ = 0;
    } else {
        scrollOffset_ = std::clamp(scrollOffset_, 0, std::max(0, VisibleNoticeCount() - ExpandedCapacity()));
    }
    baseAnim_ = mascotAnim;
    if (listChanged) {
        lastAnimationMs_ = MonotonicMs();
        SyncPills();
        ApplyLayout(true);
    }
    RefreshAnim(now);
    if (drawnFrame_ != CurrentFrame()) {
        Render(false);
    }
    ScheduleTimer();
}

void Overlay::SetActivityVisible(bool visible) {
    if (activityVisible_ == visible) {
        return;
    }
    activityVisible_ = visible;
    expanded_ = false;
    scrollOffset_ = 0;
    lastAnimationMs_ = MonotonicMs();
    ApplyLayout(true);
    ScheduleTimer();
}

void Overlay::SetLookAtCursor(bool enabled) {
    lookAtCursor_ = enabled;
    if (!enabled && lookFrame_.has_value()) {
        lookFrame_.reset();
        animator_.Restart(Ticks());
        Render(false);
    }
    ScheduleTimer();
}

void Overlay::SetDarkTheme(bool dark) {
    if (renderer_.DarkTheme() == dark) {
        return;
    }
    renderer_.SetDarkTheme(dark);
    Render(true);
}

void Overlay::PetChanged() {
    lookFrame_.reset();
    animator_.Restart(Ticks());
    frameValid_ = false;
    ApplyLayout(true);
    ScheduleTimer();
}

PetAnim Overlay::EffectiveAnim() const {
    if (transientAnim_.has_value()) {
        return *transientAnim_;
    }
    if (hoverMascot_) {
        return PetAnim::Jumping;
    }
    return baseAnim_;
}

void Overlay::RefreshAnim(int64_t now) {
    animator_.Play(EffectiveAnim(), now);
}

bool Overlay::LookEligible() const {
    if (!lookAtCursor_ || renderer_.SpriteRows() < kSpriteRowsV2 || dragging_ || hoverMascot_ || snapping_ || !shown_) {
        return false;
    }
    const PetAnim anim = EffectiveAnim();
    return anim == PetAnim::Idle || anim == PetAnim::Running || anim == PetAnim::Waving;
}

void Overlay::UpdateLook(int64_t now) {
    if (!LookEligible() || !layoutValid_) {
        if (lookFrame_.has_value()) {
            lookFrame_.reset();
            animator_.Restart(now);
        }
        return;
    }
    if (now < nextLookPollMs_) {
        return;
    }
    POINT cursor = {};
    GetCursorPos(&cursor);
    const float scale = layout_.scale;
    const float centerX = layout_.window.left + (layout_.mascot.left + layout_.mascot.right) * 0.5f * scale;
    const float centerY = layout_.window.top + (layout_.mascot.top + layout_.mascot.bottom) * 0.5f * scale;
    const float dx = cursor.x - centerX;
    const float dy = cursor.y - centerY;
    const float distance = std::hypot(dx, dy);
    const float radius = std::max(240.0f, mascotWidth_ * 2.4f) * scale;
    if (cursor.x != lastCursor_.x || cursor.y != lastCursor_.y) {
        lastCursor_ = cursor;
        lastCursorMoveMs_ = now;
    }
    nextLookPollMs_ = now + (distance <= radius * 1.3f ? 50 : 250);
    const bool active = distance <= radius && now - lastCursorMoveMs_ < kLookIdleMs && distance > mascotWidth_ * 0.25f * scale;
    std::optional<SpriteFrame> next = active ? LookFrameToward(dx, dy) : std::nullopt;
    if (next != lookFrame_) {
        const bool ended = lookFrame_.has_value() && !next.has_value();
        lookFrame_ = next;
        if (ended) {
            animator_.Restart(now);
        }
    }
}

float Overlay::ShimmerPhase(int64_t now) const {
    const int64_t position = (now - shimmerEpochMs_) % kShimmerCycleMs;
    return std::min(1.0f, static_cast<float>(position) / static_cast<float>(kShimmerSweepMs));
}

bool Overlay::HasVisibleLoading() const {
    if (!shown_) {
        return false;
    }
    for (const PillState& pill : pills_) {
        if (pill.notice.loading && pill.contentVisible && pill.opacity > 0.05f && !pill.leaving) {
            return true;
        }
    }
    return false;
}

void Overlay::Render(bool full) {
    if (window_ == nullptr) {
        return;
    }
    if (!layoutValid_) {
        ApplyLayout(true);
        return;
    }
    const int64_t now = Ticks();
    const int width = layout_.window.right - layout_.window.left;
    const int height = layout_.window.bottom - layout_.window.top;
    RECT partial = {};
    const RECT* region = nullptr;
    if (!full && frameValid_) {
        D2D1_RECT_F mascot = layout_.mascot;
        mascot.left -= 2.0f;
        mascot.top -= 2.0f;
        mascot.right += 2.0f;
        mascot.bottom += 2.0f;
        partial = DipToWindowPx(mascot);
        if (BadgeVisible()) {
            partial = UnionRect(partial, DipToWindowPx(layout_.badge));
        }
        for (const PillState& pill : pills_) {
            if (pill.notice.loading && pill.contentVisible && pill.opacity > 0.05f) {
                const D2D1_RECT_F rect = PillRect(pill);
                const D2D1_RECT_F line = D2D1::RectF(rect.left + 12.0f, rect.top + 26.0f, rect.right - 12.0f, rect.top + 46.0f);
                partial = UnionRect(partial, DipToWindowPx(line));
            }
        }
        if (partial.right > partial.left && partial.bottom > partial.top) {
            region = &partial;
        }
    }
    if (!renderer_.BeginFrame(width, height, layout_.scale, region)) {
        frameValid_ = false;
        return;
    }
    std::vector<const PillState*> order;
    order.reserve(pills_.size());
    for (const PillState& pill : pills_) {
        order.push_back(&pill);
    }
    std::stable_sort(order.begin(), order.end(), [](const PillState* a, const PillState* b) { return a->depth > b->depth; });
    const float shimmer = ShimmerPhase(now);
    for (size_t index = 0; index < order.size(); ++index) {
        const PillState& pill = *order[index];
        const int pillIndex = static_cast<int>(&pill - pills_.data());
        PillVisual visual;
        visual.rect = PillRect(pill);
        visual.opacity = std::clamp(pill.opacity, 0.0f, 1.0f);
        visual.title = pill.notice.title;
        visual.subtitle = pill.notice.subtitle;
        visual.icon = pill.notice.icon;
        visual.loading = pill.notice.loading;
        visual.shimmerPhase = shimmer;
        visual.contentVisible = pill.contentVisible;
        const bool hovered = (hover_.part == HitPart::Pill || hover_.part == HitPart::Dismiss) && hover_.pill == pillIndex;
        visual.hovered = hovered && hover_.part == HitPart::Pill;
        visual.showDismiss = hovered && !pill.leaving && pill.depth == 0;
        visual.dismissHovered = hover_.part == HitPart::Dismiss && hover_.pill == pillIndex;
        renderer_.DrawPill(visual);
    }
    const SpriteFrame frame = CurrentFrame();
    renderer_.FillHitArea(MascotHitRect());
    if (renderer_.HasSprite()) {
        renderer_.DrawPet(layout_.mascot, frame, 1.0f);
    } else {
        renderer_.DrawPetPlaceholder(layout_.mascot);
    }
    if (BadgeVisible()) {
        BadgeVisual badge;
        badge.rect = layout_.badge;
        badge.hovered = hover_.part == HitPart::Badge;
        const int count = static_cast<int>(notices_.size());
        if (!activityVisible_ || Expanded()) {
            badge.text = std::to_wstring(count);
            badge.tone = notices_.front().kind == NoticeKind::Greeting ? SessionStatus::Idle : notices_.front().status;
        } else {
            badge.chevronDown = layout_.trayAbove;
            badge.chevronUp = !layout_.trayAbove;
        }
        renderer_.DrawBadge(badge);
    }
    if (renderer_.EndFrame(window_, POINT{layout_.window.left, layout_.window.top})) {
        frameValid_ = true;
        drawnFrame_ = frame;
    } else {
        frameValid_ = false;
    }
}

}
