#include "overlay.h"

#include "log.h"
#include "overlay_metrics.h"
#include "util.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace cp {

using namespace metrics;

namespace {

bool Contains(const D2D1_RECT_F& rect, D2D1_POINT_2F point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

bool ContainsRounded(const D2D1_RECT_F& rect, D2D1_POINT_2F point) {
    if (!Contains(rect, point)) {
        return false;
    }
    const float radius = std::min(28.0f, (rect.bottom - rect.top) * 0.5f);
    const float cx = std::clamp(point.x, rect.left + radius, rect.right - radius);
    const float cy = std::clamp(point.y, rect.top + radius, rect.bottom - radius);
    const float dx = point.x - cx;
    const float dy = point.y - cy;
    return dx * dx + dy * dy <= radius * radius;
}

int64_t Ticks() {
    return static_cast<int64_t>(MonotonicMs());
}

}

Overlay::Hit Overlay::HitTest(POINT clientPx) const {
    if (!layoutValid_) {
        return {};
    }
    const D2D1_POINT_2F point = D2D1::Point2F(clientPx.x / layout_.scale, clientPx.y / layout_.scale);
    if ((hover_.part == HitPart::Pill || hover_.part == HitPart::Dismiss) && hover_.pill >= 0 && hover_.pill < static_cast<int>(pills_.size())) {
        const PillState& pill = pills_[static_cast<size_t>(hover_.pill)];
        const D2D1_RECT_F dismiss = Renderer::DismissRect(PillRect(pill));
        if (!pill.leaving && pill.depth == 0 && Contains(dismiss, point)) {
            return {HitPart::Dismiss, hover_.pill};
        }
    }
    if (const auto badge = BadgeRect(); badge.has_value() && Contains(*badge, point)) {
        return {HitPart::Badge, -1};
    }
    for (int depth = 0; depth <= kMaxPeeks; ++depth) {
        for (size_t i = 0; i < pills_.size(); ++i) {
            const PillState& pill = pills_[i];
            if (pill.leaving || pill.depth != depth || pill.opacity < 0.3f || pill.targetOpacity <= 0.0f) {
                continue;
            }
            if (ContainsRounded(PillRect(pill), point)) {
                return {pill.contentVisible && depth == 0 ? HitPart::Pill : HitPart::Peek, static_cast<int>(i)};
            }
        }
    }
    if (Contains(MascotHitRect(), point)) {
        return {HitPart::Mascot, -1};
    }
    return {};
}

void Overlay::UpdateHover(const Hit& hit) {
    const int64_t now = Ticks();
    if (LogEnabled() && !(hit == hover_)) {
        LogLine("hover " + std::to_string(static_cast<int>(hover_.part)) + ":" + std::to_string(hover_.pill) + " -> " + std::to_string(static_cast<int>(hit.part)) + ":" +
                std::to_string(hit.pill) + " expanded=" + std::to_string(expanded_));
    }
    const bool mascotHover = hit.part == HitPart::Mascot;
    bool changed = false;
    if (mascotHover != hoverMascot_) {
        hoverMascot_ = mascotHover;
        if (hoverMascot_ && lookFrame_.has_value()) {
            lookFrame_.reset();
        }
        RefreshAnim(now);
        changed = true;
    }
    const bool trayHover = hit.part == HitPart::Pill || hit.part == HitPart::Peek || hit.part == HitPart::Dismiss || hit.part == HitPart::Badge;
    if (trayHover && hit.part != HitPart::Badge && VisibleNoticeCount() > 1) {
        if (collapsePending_) {
            KillTimer(window_, kCollapseTimer);
            collapsePending_ = false;
        }
        if (!expanded_) {
            hover_ = hit;
            SetExpanded(true);
            return;
        }
    } else if (trayHover && collapsePending_) {
        KillTimer(window_, kCollapseTimer);
        collapsePending_ = false;
    } else if (!trayHover && expanded_ && !collapsePending_) {
        collapsePending_ = true;
        SetTimer(window_, kCollapseTimer, kCollapseDelayMs, nullptr);
    }
    if (!(hit == hover_)) {
        hover_ = hit;
        changed = true;
    }
    if (changed) {
        Render(true);
        ScheduleTimer();
    }
}

void Overlay::PollHover() {
    if (pressed_ || !layoutValid_ || (hover_.part == HitPart::None && !hoverMascot_ && !expanded_)) {
        return;
    }
    POINT cursor = {};
    GetCursorPos(&cursor);
    const RECT& window = layout_.window;
    Hit hit;
    if (cursor.x >= window.left && cursor.x < window.right && cursor.y >= window.top && cursor.y < window.bottom) {
        hit = HitTest(POINT{cursor.x - window.left, cursor.y - window.top});
    }
    const bool trayHit = hit.part == HitPart::Pill || hit.part == HitPart::Peek || hit.part == HitPart::Dismiss || hit.part == HitPart::Badge;
    const bool collapseNeeded = expanded_ && !trayHit && !collapsePending_;
    if (!(hit == hover_) || (hit.part == HitPart::Mascot) != hoverMascot_ || collapseNeeded) {
        UpdateHover(hit);
    }
}

void Overlay::FinishPressIfReleased() {
    if (!pressed_) {
        return;
    }
    const int primary = GetSystemMetrics(SM_SWAPBUTTON) != 0 ? VK_RBUTTON : VK_LBUTTON;
    if ((GetAsyncKeyState(primary) & 0x8000) != 0) {
        return;
    }
    POINT cursor = {};
    GetCursorPos(&cursor);
    ScreenToClient(window_, &cursor);
    LogLine("button released without WM_LBUTTONUP");
    OnLeftUp(cursor);
}

void Overlay::OnMouseMove(POINT clientPx, WPARAM keys) {
    if (pressed_ && (keys & MK_LBUTTON) == 0) {
        LogLine("mouse move without button during press");
        OnLeftUp(clientPx);
        return;
    }
    if (!trackingLeave_) {
        TRACKMOUSEEVENT track = {};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = window_;
        trackingLeave_ = TrackMouseEvent(&track) != FALSE;
    }
    if (pressed_ && pressHit_.part == HitPart::Mascot) {
        POINT cursor = {};
        GetCursorPos(&cursor);
        const LONG dx = cursor.x - pressCursor_.x;
        const LONG dy = cursor.y - pressCursor_.y;
        if (!dragging_ && (std::abs(dx) >= kDragThresholdPx || std::abs(dy) >= kDragThresholdPx)) {
            dragging_ = true;
            snapping_ = false;
            lookFrame_.reset();
            hoverMascot_ = false;
            lastDragX_ = pressCursor_.x;
            ScheduleTimer();
        }
        return;
    }
    UpdateHover(HitTest(clientPx));
}

void Overlay::OnMouseLeave() {
    trackingLeave_ = false;
    LogLine("mouse leave");
    if (pressed_) {
        return;
    }
    UpdateHover(Hit{});
}

void Overlay::OnLeftDown(POINT clientPx) {
    const Hit hit = HitTest(clientPx);
    if (LogEnabled()) {
        LogLine("left down at " + std::to_string(clientPx.x) + "," + std::to_string(clientPx.y) + " part=" + std::to_string(static_cast<int>(hit.part)));
    }
    if (hit.part == HitPart::None) {
        return;
    }
    pressed_ = true;
    dragging_ = false;
    pressHit_ = hit;
    GetCursorPos(&pressCursor_);
    if (hit.part == HitPart::Mascot && layoutValid_) {
        const float scale = layout_.scale;
        const float mascotW = mascotWidth_ * scale;
        const float mascotH = MascotHeightDip() * scale;
        grabFractionX_ = std::clamp((pressCursor_.x - anchor_.x) / mascotW, 0.0f, 1.0f);
        grabFractionY_ = std::clamp((pressCursor_.y - anchor_.y) / mascotH, 0.0f, 1.0f);
        transientAnim_.reset();
        snapping_ = false;
    }
    SetCapture(window_);
    ScheduleTimer();
}

void Overlay::OnLeftUp(POINT clientPx) {
    if (LogEnabled()) {
        LogLine("left up at " + std::to_string(clientPx.x) + "," + std::to_string(clientPx.y) + " pressed=" + std::to_string(pressed_) + " dragging=" + std::to_string(dragging_));
    }
    if (!pressed_) {
        return;
    }
    const bool wasDragging = dragging_;
    const Hit pressed = pressHit_;
    pressed_ = false;
    dragging_ = false;
    if (GetCapture() == window_) {
        ReleaseCapture();
    }
    const int64_t now = Ticks();
    if (wasDragging) {
        POINT cursor = {};
        GetCursorPos(&cursor);
        MoveAnchor(DragAnchor(cursor));
        transientAnim_.reset();
        RefreshAnim(now);
        if (GetKeyState(VK_MENU) < 0) {
            StartSnap(cursor);
        } else {
            PersistPosition();
        }
        Render(true);
        ScheduleTimer();
        UpdateHover(HitTest(clientPx));
        return;
    }
    const Hit released = HitTest(clientPx);
    if (pressed.part != HitPart::Mascot && (released.part != pressed.part || released.pill != pressed.pill)) {
        return;
    }
    switch (pressed.part) {
    case HitPart::Mascot:
        if (callbacks_.mascotClicked) {
            callbacks_.mascotClicked();
        }
        break;
    case HitPart::Badge:
        ActivateBadge();
        break;
    case HitPart::Peek:
        SetExpanded(true);
        break;
    case HitPart::Pill:
        if (pressed.pill >= 0 && pressed.pill < static_cast<int>(pills_.size()) && callbacks_.openNotice) {
            const Notice notice = pills_[static_cast<size_t>(pressed.pill)].notice;
            callbacks_.openNotice(notice);
        }
        break;
    case HitPart::Dismiss:
        if (pressed.pill >= 0 && pressed.pill < static_cast<int>(pills_.size()) && callbacks_.dismissNotice) {
            const Notice notice = pills_[static_cast<size_t>(pressed.pill)].notice;
            hover_ = Hit{};
            callbacks_.dismissNotice(notice);
        }
        break;
    case HitPart::None:
        break;
    }
}

void Overlay::OnRightUp(POINT clientPx) {
    const Hit hit = HitTest(clientPx);
    POINT screen = clientPx;
    ClientToScreen(window_, &screen);
    if ((hit.part == HitPart::Pill || hit.part == HitPart::Dismiss) && hit.pill >= 0 && hit.pill < static_cast<int>(pills_.size())) {
        if (callbacks_.noticeMenu) {
            const Notice notice = pills_[static_cast<size_t>(hit.pill)].notice;
            callbacks_.noticeMenu(notice, screen);
        }
        return;
    }
    if (callbacks_.contextMenu) {
        callbacks_.contextMenu(screen);
    }
}

void Overlay::OnWheel(int delta) {
    if (!Expanded()) {
        return;
    }
    const int count = VisibleNoticeCount();
    const int capacity = ExpandedCapacity();
    if (count <= capacity) {
        return;
    }
    const int direction = (delta > 0) == layout_.trayAbove ? 1 : -1;
    const int next = std::clamp(scrollOffset_ + direction, 0, count - capacity);
    if (next == scrollOffset_) {
        return;
    }
    scrollOffset_ = next;
    lastAnimationMs_ = MonotonicMs();
    UpdatePillTargets();
    Render(true);
    ScheduleTimer();
}

void Overlay::OnCaptureLost() {
    if (LogEnabled()) {
        LogLine("capture lost pressed=" + std::to_string(pressed_) + " dragging=" + std::to_string(dragging_));
    }
    if (!pressed_) {
        return;
    }
    const bool wasDragging = dragging_;
    pressed_ = false;
    dragging_ = false;
    if (wasDragging) {
        POINT cursor = {};
        GetCursorPos(&cursor);
        MoveAnchor(DragAnchor(cursor));
        transientAnim_.reset();
        RefreshAnim(Ticks());
        PersistPosition();
        Render(true);
        ScheduleTimer();
    }
}

bool Overlay::OnSetCursor() {
    if (dragging_) {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return true;
    }
    POINT cursor = {};
    GetCursorPos(&cursor);
    ScreenToClient(window_, &cursor);
    const Hit hit = HitTest(cursor);
    SetCursor(LoadCursorW(nullptr, hit.part == HitPart::None ? IDC_ARROW : IDC_HAND));
    return true;
}

void Overlay::StartSnap(POINT cursor) {
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const RECT& work = info.rcWork;
    const float scale = layout_.scale;
    const float width = mascotWidth_ * scale;
    const float height = MascotHeightDip() * scale;
    const float workWidth = static_cast<float>(work.right - work.left);
    const float fraction = (cursor.x - work.left) / std::max(1.0f, workWidth);
    const float margin = kSnapMargin * scale;
    const float span = std::min(width, workWidth - 2.0f * margin);
    float centerX = (work.left + work.right) * 0.5f;
    if (fraction < 1.0f / 3.0f) {
        centerX = work.left + margin + span * 0.5f;
    } else if (fraction >= 2.0f / 3.0f) {
        centerX = work.right - margin - span * 0.5f;
    }
    const bool top = cursor.y < (work.top + work.bottom) * 0.5f;
    const float centerY = top ? work.top + margin + height * 0.5f : work.bottom - margin - height * 0.5f;
    snapTarget_ = POINT{static_cast<LONG>(std::lround(centerX - width * 0.5f)), static_cast<LONG>(std::lround(centerY - height * 0.5f))};
    snapX_ = static_cast<float>(anchor_.x);
    snapY_ = static_cast<float>(anchor_.y);
    snapVx_ = 0.0f;
    snapVy_ = 0.0f;
    snapStartMs_ = MonotonicMs();
    snapLastMs_ = snapStartMs_;
    snapping_ = true;
}

void Overlay::PersistPosition() {
    if (callbacks_.positionChanged) {
        callbacks_.positionChanged(MonitorKey(), anchor_);
    }
}

void Overlay::ActivateBadge() {
    if (!activityVisible_) {
        if (callbacks_.activityVisibilityRequested) {
            callbacks_.activityVisibilityRequested(true);
        }
        return;
    }
    if (Expanded()) {
        SetExpanded(false);
        return;
    }
    if (callbacks_.activityVisibilityRequested) {
        callbacks_.activityVisibilityRequested(false);
    }
}

}
