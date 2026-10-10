#include "overlay.h"

#include "overlay_metrics.h"
#include "util.h"

#include <algorithm>
#include <cmath>

namespace cp {

using namespace metrics;

namespace {

RectF Shift(const RectF& rect, float dx, float dy) {
    return RectF{rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy};
}

bool Near(float a, float b) {
    return std::fabs(a - b) < 0.01f;
}

bool SameRect(const RectF& a, const RectF& b) {
    return Near(a.left, b.left) && Near(a.top, b.top) && Near(a.right, b.right) && Near(a.bottom, b.bottom);
}

}

float Overlay::MascotHeightDip() const {
    return std::ceil(static_cast<float>(mascotWidth_) / kCellAspect);
}

int Overlay::VisibleNoticeCount() const {
    return activityVisible_ ? static_cast<int>(notices_.size()) : 0;
}

int Overlay::ExpandedCapacity() const {
    return std::min(VisibleNoticeCount(), kMaxExpanded);
}

bool Overlay::Expanded() const {
    return expanded_ && VisibleNoticeCount() > 1;
}

void Overlay::SetExpanded(bool expanded) {
    if (expanded_ == expanded) {
        return;
    }
    expanded_ = expanded;
    if (!expanded_) {
        scrollOffset_ = 0;
    }
    lastAnimationMs_ = MonotonicMs();
    ApplyLayout(true);
    ScheduleTimer();
}

Overlay::Layout Overlay::ComputeLayout() const {
    Layout layout;
    DisplayInfo display = DisplayFromPoint(PointI{anchor_.x + 1, anchor_.y + 1});
    float scale = display.scale;
    const float mascotHeight = MascotHeightDip();
    const PointI center = {anchor_.x + static_cast<int32_t>(mascotWidth_ * scale * 0.5f), anchor_.y + static_cast<int32_t>(mascotHeight * scale * 0.5f)};
    DisplayInfo centerDisplay = DisplayFromPoint(center);
    if (centerDisplay.id != display.id) {
        display = std::move(centerDisplay);
        scale = display.scale;
    }
    layout.monitor = display.id;
    layout.scale = scale;
    layout.work = display.work;

    const float mascotX = static_cast<float>(anchor_.x);
    const float mascotY = static_cast<float>(anchor_.y);
    const float mascotW = mascotWidth_ * scale;
    const float mascotH = mascotHeight * scale;
    const RectI& work = layout.work;

    int visiblePills = 0;
    for (const PillState& pill : pills_) {
        if (pill.targetOpacity > 0.0f || pill.opacity > 0.01f) {
            ++visiblePills;
        }
    }
    int capacity = std::max(ExpandedCapacity(), std::min(visiblePills, kMaxExpanded));
    if (visiblePills == 0 && VisibleNoticeCount() == 0) {
        capacity = 0;
    }

    const float workWidth = static_cast<float>(work.right - work.left);
    const float trayWidth = std::max(120.0f * scale, std::min(kTrayWidth * scale, workWidth - 2.0f * kScreenMargin * scale));
    const float minTrayX = work.left + kScreenMargin * scale;
    const float maxTrayX = work.right - kScreenMargin * scale - trayWidth;
    float trayX = mascotX + mascotW * 0.5f - trayWidth * 0.5f;
    trayX = maxTrayX < minTrayX ? minTrayX : std::clamp(trayX, minTrayX, maxTrayX);
    const float centerOffset = mascotY + mascotH * 0.5f - (work.top + work.bottom) * 0.5f;
    bool trayAbove = centerOffset > 0.0f;
    if (layoutValid_ && std::fabs(centerOffset) < kTrayFlipHysteresis * scale) {
        trayAbove = layout_.trayAbove;
    }
    const float trayHeight = capacity > 0 ? (capacity * kPillHeight + (capacity - 1) * kPillGap + kMaxPeeks * kPeekOffset) * scale : 0.0f;
    const float nearEdge = trayAbove ? mascotY - kTrayGap * scale : mascotY + mascotH + kTrayGap * scale;
    const float trayTop = trayAbove ? nearEdge - trayHeight : nearEdge;

    float left = mascotX;
    float top = mascotY;
    float right = mascotX + mascotW;
    float bottom = mascotY + mascotH;
    if (capacity > 0) {
        const float slackX = kLagSlackX * scale;
        const float slackY = kLagSlackY * scale;
        left = std::min(left, trayX - slackX);
        right = std::max(right, trayX + trayWidth + slackX);
        top = std::min(top, trayAbove ? trayTop - slackY : trayTop);
        bottom = std::max(bottom, trayAbove ? trayTop + trayHeight : trayTop + trayHeight + slackY);
    }
    const float margin = kWindowMargin * scale;
    layout.window.left = static_cast<int32_t>(std::floor(left - margin));
    layout.window.top = static_cast<int32_t>(std::floor(top - margin));
    layout.window.right = static_cast<int32_t>(std::ceil(right + margin));
    layout.window.bottom = static_cast<int32_t>(std::ceil(bottom + margin));

    const float originX = static_cast<float>(layout.window.left);
    const float originY = static_cast<float>(layout.window.top);
    layout.mascot = RectF{(mascotX - originX) / scale, (mascotY - originY) / scale, (mascotX - originX) / scale + mascotWidth_,
                          (mascotY - originY) / scale + mascotHeight};
    layout.trayLeft = (trayX - originX) / scale;
    layout.trayWidth = trayWidth / scale;
    layout.trayNearEdge = (nearEdge - originY) / scale;
    layout.trayAbove = trayAbove;
    return layout;
}

Overlay::LayoutChange Overlay::Relayout() {
    const Layout next = ComputeLayout();
    window_.SetFrameClockDisplay(next.monitor);
    if (!layoutValid_) {
        layout_ = next;
        layoutValid_ = true;
        lag_ = {};
        UpdatePillTargets();
        return LayoutChange::Geometry;
    }
    const bool flipped = next.trayAbove != layout_.trayAbove;
    if (flipped || !Near(next.scale, layout_.scale)) {
        layout_ = next;
        lag_ = {};
        UpdatePillTargets();
        const float entrance = layout_.trayAbove ? kEntranceOffset : -kEntranceOffset;
        for (PillState& pill : pills_) {
            pill.rect = flipped ? Shift(pill.target, 0.0f, entrance) : pill.target;
            if (flipped) {
                pill.opacity = 0.0f;
            }
        }
        return LayoutChange::Geometry;
    }
    const float dx = static_cast<float>(layout_.window.left - next.window.left) / next.scale;
    const float dy = static_cast<float>(layout_.window.top - next.window.top) / next.scale;
    const float trayDx = next.trayLeft - (layout_.trayLeft + dx);
    const float trayDy = next.trayNearEdge - (layout_.trayNearEdge + dy);
    const float shiftX = dx + trayDx;
    const float shiftY = dy + trayDy;
    if (shiftX != 0.0f || shiftY != 0.0f) {
        for (PillState& pill : pills_) {
            pill.rect = Shift(pill.rect, shiftX, shiftY);
            pill.target = Shift(pill.target, shiftX, shiftY);
        }
    }
    const bool resized = layout_.window.right - layout_.window.left != next.window.right - next.window.left ||
                         layout_.window.bottom - layout_.window.top != next.window.bottom - next.window.top;
    const bool moved = layout_.window.left != next.window.left || layout_.window.top != next.window.top;
    const bool reshaped = resized || !SameRect(layout_.mascot, next.mascot) || !Near(layout_.trayLeft, next.trayLeft) || !Near(layout_.trayNearEdge, next.trayNearEdge);
    const PointF lagBefore = lag_;
    layout_ = next;
    UpdatePillTargets();
    if (trayDx != 0.0f || trayDy != 0.0f) {
        ShiftLag(-trayDx, -trayDy);
    }
    const bool pillsShifted = PillsVisible() && (!Near(shiftX + lag_.x - lagBefore.x, 0.0f) || !Near(shiftY + lag_.y - lagBefore.y, 0.0f));
    if (reshaped || pillsShifted) {
        return LayoutChange::Geometry;
    }
    return moved ? LayoutChange::Moved : LayoutChange::None;
}

void Overlay::ApplyLayout(bool forceRender) {
    if (!window_.Created()) {
        return;
    }
    const LayoutChange change = Relayout();
    if (forceRender || change == LayoutChange::Geometry || !frameValid_) {
        Render(true);
    } else if (change == LayoutChange::Moved && shown_) {
        window_.Move(PointI{layout_.window.left, layout_.window.top});
    }
}

void Overlay::SyncPills() {
    std::vector<PillState> next;
    next.reserve(notices_.size() + pills_.size());
    std::vector<bool> used(pills_.size(), false);
    std::vector<size_t> fresh;
    for (const Notice& notice : notices_) {
        bool matched = false;
        for (size_t i = 0; i < pills_.size(); ++i) {
            if (!used[i] && pills_[i].notice.id == notice.id) {
                used[i] = true;
                PillState state = pills_[i];
                state.notice = notice;
                state.leaving = false;
                next.push_back(std::move(state));
                matched = true;
                break;
            }
        }
        if (!matched) {
            PillState state;
            state.notice = notice;
            fresh.push_back(next.size());
            next.push_back(std::move(state));
        }
    }
    for (size_t i = 0; i < pills_.size(); ++i) {
        if (!used[i] && pills_[i].opacity > 0.01f) {
            PillState state = pills_[i];
            state.leaving = true;
            state.targetOpacity = 0.0f;
            next.push_back(std::move(state));
        }
    }
    pills_ = std::move(next);
    if (!layoutValid_) {
        return;
    }
    UpdatePillTargets();
    const float entrance = layout_.trayAbove ? kEntranceOffset : -kEntranceOffset;
    for (const size_t index : fresh) {
        PillState& pill = pills_[index];
        pill.rect = Shift(pill.target, 0.0f, entrance);
        pill.opacity = 0.0f;
    }
}

void Overlay::UpdatePillTargets() {
    if (!layoutValid_) {
        return;
    }
    const bool expanded = Expanded();
    const int capacity = std::max(1, ExpandedCapacity());
    auto slot = [&](int k) {
        const float y = layout_.trayAbove ? layout_.trayNearEdge - (k + 1) * kPillHeight - k * kPillGap : layout_.trayNearEdge + k * (kPillHeight + kPillGap);
        return RectF{layout_.trayLeft, y, layout_.trayLeft + layout_.trayWidth, y + kPillHeight};
    };
    int index = 0;
    for (PillState& pill : pills_) {
        if (pill.leaving) {
            pill.targetOpacity = 0.0f;
            pill.depth = kMaxPeeks + 1;
            continue;
        }
        const int i = index++;
        if (!activityVisible_) {
            pill.target = slot(0);
            pill.targetOpacity = 0.0f;
            pill.depth = i;
            pill.contentVisible = i == 0;
            continue;
        }
        if (expanded) {
            const int k = i - scrollOffset_;
            const bool inside = k >= 0 && k < capacity;
            pill.target = slot(std::clamp(k, 0, capacity - 1));
            pill.targetOpacity = inside ? 1.0f : 0.0f;
            pill.depth = inside ? 0 : kMaxPeeks + 1;
            pill.contentVisible = true;
            continue;
        }
        const int depth = std::min(i, kMaxPeeks);
        RectF rect = slot(0);
        if (depth > 0) {
            const float shift = depth * kPeekOffset * (layout_.trayAbove ? -1.0f : 1.0f);
            rect = RectF{rect.left + depth * kPeekInset, rect.top + shift, rect.right - depth * kPeekInset, rect.bottom + shift};
        }
        pill.target = rect;
        pill.targetOpacity = i == 0 ? 1.0f : (i <= kMaxPeeks ? 1.0f - 0.22f * i : 0.0f);
        pill.depth = depth;
        pill.contentVisible = i == 0;
    }
}

bool Overlay::PillsVisible() const {
    for (const PillState& pill : pills_) {
        if (pill.opacity > 0.01f || pill.targetOpacity > 0.0f) {
            return true;
        }
    }
    return false;
}

RectF Overlay::PillRect(const PillState& pill) const {
    return Shift(pill.rect, lag_.x, lag_.y);
}

std::optional<RectF> Overlay::BadgeRect() const {
    if (!BadgeVisible()) {
        return std::nullopt;
    }
    return BadgeArea();
}

RectF Overlay::BadgeArea() const {
    const RectF& mascot = layout_.mascot;
    if (!activityVisible_ || !PillsVisible()) {
        return RectF{mascot.right - kBadgeSize, mascot.top, mascot.right, mascot.top + kBadgeSize};
    }
    const float half = kBadgeSize * 0.5f;
    const float corner = kPillHeight * 0.5f;
    const float minX = layout_.trayLeft + corner + half;
    const float maxX = std::max(minX, layout_.trayLeft + layout_.trayWidth - corner - half);
    const float centerX = std::clamp(mascot.right - half, minX, maxX) + lag_.x;
    const float centerY = layout_.trayNearEdge + (layout_.trayAbove ? kBadgeDockOffset : -kBadgeDockOffset) + lag_.y;
    return RectF{centerX - half, centerY - half, centerX + half, centerY + half};
}

bool Overlay::BadgeWanted() const {
    return hoverMascot_ || hover_.part == HitPart::Pill || hover_.part == HitPart::Peek || hover_.part == HitPart::Dismiss || hover_.part == HitPart::Badge;
}

bool Overlay::BadgeVisible() const {
    if (notices_.empty() || !layoutValid_) {
        return false;
    }
    if (!activityVisible_ || BadgeWanted()) {
        return true;
    }
    return badgeHoldUntilMs_ != 0 && static_cast<int64_t>(MonotonicMs()) < badgeHoldUntilMs_;
}

RectF Overlay::MascotHitRect() const {
    const RectF& mascot = layout_.mascot;
    const RectF& bounds = renderer_.SpriteContentBounds();
    const float width = mascot.right - mascot.left;
    const float height = mascot.bottom - mascot.top;
    return RectF{mascot.left + bounds.left * width - 2.0f, mascot.top + bounds.top * height - 2.0f, mascot.left + bounds.right * width + 2.0f,
                 mascot.top + bounds.bottom * height + 2.0f};
}

RectI Overlay::DipToWindowPx(const RectF& rect) const {
    const float s = layout_.scale;
    const int32_t width = layout_.window.right - layout_.window.left;
    const int32_t height = layout_.window.bottom - layout_.window.top;
    RectI result = {static_cast<int32_t>(std::floor(rect.left * s)), static_cast<int32_t>(std::floor(rect.top * s)), static_cast<int32_t>(std::ceil(rect.right * s)),
                    static_cast<int32_t>(std::ceil(rect.bottom * s))};
    result.left = std::clamp<int32_t>(result.left, 0, width);
    result.right = std::clamp<int32_t>(result.right, 0, width);
    result.top = std::clamp<int32_t>(result.top, 0, height);
    result.bottom = std::clamp<int32_t>(result.bottom, 0, height);
    return result;
}

PointI Overlay::ClampAnchor(PointI anchor, const RectI& work, float scale) const {
    const int32_t width = static_cast<int32_t>(std::lround(mascotWidth_ * scale));
    const int32_t height = static_cast<int32_t>(std::lround(MascotHeightDip() * scale));
    const int32_t reserve = static_cast<int32_t>(std::lround(kBottomReserve * scale));
    const int32_t maxX = std::max(work.left, work.right - width);
    const int32_t maxY = std::max(work.top, work.bottom - height - reserve);
    return PointI{std::clamp(anchor.x, work.left, maxX), std::clamp(anchor.y, work.top, maxY)};
}

std::wstring Overlay::MonitorKey() const {
    const int32_t width = static_cast<int32_t>(mascotWidth_ * layout_.scale);
    const int32_t height = static_cast<int32_t>(MascotHeightDip() * layout_.scale);
    return DisplayKey(DisplayFromPoint(PointI{anchor_.x + width / 2, anchor_.y + height / 2}).id);
}

void Overlay::PlaceInitial(const Settings& settings) {
    if (!settings.lastMonitorKey.empty()) {
        if (const std::optional<DisplayInfo> display = FindDisplay(settings.lastMonitorKey)) {
            if (const SavedPosition* saved = FindPosition(settings, settings.lastMonitorKey)) {
                anchor_ = ClampAnchor(saved->anchor, display->work, display->scale);
                layoutValid_ = false;
                return;
            }
        }
    }
    const DisplayInfo primary = PrimaryDisplay();
    const float scale = primary.scale;
    const int32_t width = static_cast<int32_t>(std::lround(mascotWidth_ * scale));
    const int32_t height = static_cast<int32_t>(std::lround(MascotHeightDip() * scale));
    const int32_t margin = static_cast<int32_t>(std::lround(kDefaultEdgeMargin * scale));
    anchor_ = PointI{primary.work.right - width - margin, primary.work.bottom - height - margin};
    layoutValid_ = false;
}

void Overlay::HandleDisplayChange() {
    if (!window_.Created()) {
        return;
    }
    const Layout probe = ComputeLayout();
    if (!dragging_) {
        anchor_ = ClampAnchor(anchor_, probe.work, probe.scale);
    }
    frameValid_ = false;
    ApplyLayout(true);
    ScheduleTimer();
}

void Overlay::SetMascotWidth(int widthDip) {
    const int clamped = std::clamp(widthDip, kMinMascotWidth, kMaxMascotWidth);
    if (clamped == mascotWidth_) {
        return;
    }
    const float scale = layoutValid_ ? layout_.scale : 1.0f;
    const int32_t oldWidth = static_cast<int32_t>(mascotWidth_ * scale);
    const int32_t oldHeight = static_cast<int32_t>(MascotHeightDip() * scale);
    mascotWidth_ = clamped;
    const int32_t newWidth = static_cast<int32_t>(mascotWidth_ * scale);
    const int32_t newHeight = static_cast<int32_t>(MascotHeightDip() * scale);
    anchor_.x += (oldWidth - newWidth) / 2;
    anchor_.y += oldHeight - newHeight;
    if (layoutValid_) {
        anchor_ = ClampAnchor(anchor_, layout_.work, layout_.scale);
    }
    frameValid_ = false;
    ApplyLayout(true);
    ScheduleTimer();
}

}
