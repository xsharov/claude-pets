#include "overlay.h"

#include "overlay_metrics.h"
#include "util.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>

namespace cp {

using namespace metrics;

namespace {

float MonitorScale(HMONITOR monitor) {
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) {
        dpiX = 96;
    }
    return static_cast<float>(dpiX) / 96.0f;
}

std::wstring KeyForMonitor(HMONITOR monitor) {
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return {};
    }
    const RECT& r = info.rcMonitor;
    return std::wstring(info.szDevice) + L"@" + std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," + std::to_wstring(r.right) + L"," +
           std::to_wstring(r.bottom);
}

D2D1_RECT_F Shift(const D2D1_RECT_F& rect, float dx, float dy) {
    return D2D1::RectF(rect.left + dx, rect.top + dy, rect.right + dx, rect.bottom + dy);
}

bool Near(float a, float b) {
    return std::fabs(a - b) < 0.01f;
}

bool SameRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    return Near(a.left, b.left) && Near(a.top, b.top) && Near(a.right, b.right) && Near(a.bottom, b.bottom);
}

struct MonitorSearch {
    std::wstring key;
    HMONITOR found = nullptr;
};

BOOL CALLBACK FindMonitorByKey(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* search = reinterpret_cast<MonitorSearch*>(parameter);
    if (KeyForMonitor(monitor) == search->key) {
        search->found = monitor;
        return FALSE;
    }
    return TRUE;
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
    HMONITOR monitor = MonitorFromPoint(POINT{anchor_.x + 1, anchor_.y + 1}, MONITOR_DEFAULTTONEAREST);
    float scale = MonitorScale(monitor);
    const float mascotHeight = MascotHeightDip();
    const POINT center = {anchor_.x + static_cast<LONG>(mascotWidth_ * scale * 0.5f), anchor_.y + static_cast<LONG>(mascotHeight * scale * 0.5f)};
    const HMONITOR centerMonitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    if (centerMonitor != monitor) {
        monitor = centerMonitor;
        scale = MonitorScale(monitor);
    }
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    layout.monitor = monitor;
    layout.scale = scale;
    layout.work = info.rcWork;

    const float mascotX = static_cast<float>(anchor_.x);
    const float mascotY = static_cast<float>(anchor_.y);
    const float mascotW = mascotWidth_ * scale;
    const float mascotH = mascotHeight * scale;
    const RECT& work = layout.work;

    int visiblePills = 0;
    for (const PillState& pill : pills_) {
        if (pill.targetOpacity > 0.0f || pill.opacity > 0.01f) {
            ++visiblePills;
        }
    }
    int capacity = std::max(ExpandedCapacity(), std::min(visiblePills, kMaxExpanded));
    if (visiblePills == 0) {
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
    layout.window.left = static_cast<LONG>(std::floor(left - margin));
    layout.window.top = static_cast<LONG>(std::floor(top - margin));
    layout.window.right = static_cast<LONG>(std::ceil(right + margin));
    layout.window.bottom = static_cast<LONG>(std::ceil(bottom + margin));

    const float originX = static_cast<float>(layout.window.left);
    const float originY = static_cast<float>(layout.window.top);
    layout.mascot = D2D1::RectF((mascotX - originX) / scale, (mascotY - originY) / scale, (mascotX - originX) / scale + mascotWidth_,
                                (mascotY - originY) / scale + mascotHeight);
    layout.trayLeft = (trayX - originX) / scale;
    layout.trayWidth = trayWidth / scale;
    layout.trayNearEdge = (nearEdge - originY) / scale;
    layout.trayAbove = trayAbove;
    layout.badge = D2D1::RectF(layout.mascot.right - kBadgeSize, layout.mascot.top, layout.mascot.right, layout.mascot.top + kBadgeSize);
    return layout;
}

Overlay::LayoutChange Overlay::Relayout() {
    const Layout next = ComputeLayout();
    frameClock_.SetMonitor(next.monitor);
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
    const D2D1_POINT_2F lagBefore = lag_;
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
    if (window_ == nullptr) {
        return;
    }
    const LayoutChange change = Relayout();
    if (forceRender || change == LayoutChange::Geometry || !frameValid_) {
        Render(true);
    } else if (change == LayoutChange::Moved && shown_) {
        SetWindowPos(window_, nullptr, layout_.window.left, layout_.window.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
        return D2D1::RectF(layout_.trayLeft, y, layout_.trayLeft + layout_.trayWidth, y + kPillHeight);
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
        D2D1_RECT_F rect = slot(0);
        if (depth > 0) {
            const float shift = depth * kPeekOffset * (layout_.trayAbove ? -1.0f : 1.0f);
            rect = D2D1::RectF(rect.left + depth * kPeekInset, rect.top + shift, rect.right - depth * kPeekInset, rect.bottom + shift);
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

D2D1_RECT_F Overlay::PillRect(const PillState& pill) const {
    return Shift(pill.rect, lag_.x, lag_.y);
}

std::optional<D2D1_RECT_F> Overlay::BadgeRect() const {
    if (!BadgeVisible()) {
        return std::nullopt;
    }
    return layout_.badge;
}

bool Overlay::BadgeVisible() const {
    if (notices_.empty() || !layoutValid_) {
        return false;
    }
    if (!activityVisible_) {
        return true;
    }
    return hoverMascot_ || hover_.part == HitPart::Pill || hover_.part == HitPart::Peek || hover_.part == HitPart::Dismiss || hover_.part == HitPart::Badge;
}

D2D1_RECT_F Overlay::MascotHitRect() const {
    const D2D1_RECT_F& mascot = layout_.mascot;
    const D2D1_RECT_F& bounds = renderer_.SpriteContentBounds();
    const float width = mascot.right - mascot.left;
    const float height = mascot.bottom - mascot.top;
    return D2D1::RectF(mascot.left + bounds.left * width - 2.0f, mascot.top + bounds.top * height - 2.0f, mascot.left + bounds.right * width + 2.0f,
                       mascot.top + bounds.bottom * height + 2.0f);
}

RECT Overlay::DipToWindowPx(const D2D1_RECT_F& rect) const {
    const float s = layout_.scale;
    const LONG width = layout_.window.right - layout_.window.left;
    const LONG height = layout_.window.bottom - layout_.window.top;
    RECT result = {static_cast<LONG>(std::floor(rect.left * s)), static_cast<LONG>(std::floor(rect.top * s)), static_cast<LONG>(std::ceil(rect.right * s)),
                   static_cast<LONG>(std::ceil(rect.bottom * s))};
    result.left = std::clamp<LONG>(result.left, 0, width);
    result.right = std::clamp<LONG>(result.right, 0, width);
    result.top = std::clamp<LONG>(result.top, 0, height);
    result.bottom = std::clamp<LONG>(result.bottom, 0, height);
    return result;
}

POINT Overlay::ClampAnchor(POINT anchor, const RECT& work, float scale) const {
    const LONG width = static_cast<LONG>(std::lround(mascotWidth_ * scale));
    const LONG height = static_cast<LONG>(std::lround(MascotHeightDip() * scale));
    const LONG reserve = static_cast<LONG>(std::lround(kBottomReserve * scale));
    const LONG maxX = std::max(work.left, work.right - width);
    const LONG maxY = std::max(work.top, work.bottom - height - reserve);
    return POINT{std::clamp(anchor.x, work.left, maxX), std::clamp(anchor.y, work.top, maxY)};
}

std::wstring Overlay::MonitorKey() const {
    const LONG width = static_cast<LONG>(mascotWidth_ * layout_.scale);
    const LONG height = static_cast<LONG>(MascotHeightDip() * layout_.scale);
    return KeyForMonitor(MonitorFromPoint(POINT{anchor_.x + width / 2, anchor_.y + height / 2}, MONITOR_DEFAULTTONEAREST));
}

void Overlay::PlaceInitial(const Settings& settings) {
    MonitorSearch search;
    search.key = settings.lastMonitorKey;
    if (!search.key.empty()) {
        EnumDisplayMonitors(nullptr, nullptr, FindMonitorByKey, reinterpret_cast<LPARAM>(&search));
    }
    if (search.found != nullptr) {
        if (const SavedPosition* saved = FindPosition(settings, search.key)) {
            MONITORINFO info = {};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(search.found, &info);
            anchor_ = ClampAnchor(saved->anchor, info.rcWork, MonitorScale(search.found));
            layoutValid_ = false;
            return;
        }
    }
    const HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(primary, &info);
    const float scale = MonitorScale(primary);
    const LONG width = static_cast<LONG>(std::lround(mascotWidth_ * scale));
    const LONG height = static_cast<LONG>(std::lround(MascotHeightDip() * scale));
    const LONG margin = static_cast<LONG>(std::lround(kDefaultEdgeMargin * scale));
    anchor_ = POINT{info.rcWork.right - width - margin, info.rcWork.bottom - height - margin};
    layoutValid_ = false;
}

void Overlay::HandleDisplayChange() {
    if (window_ == nullptr) {
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
    const LONG oldWidth = static_cast<LONG>(mascotWidth_ * scale);
    const LONG oldHeight = static_cast<LONG>(MascotHeightDip() * scale);
    mascotWidth_ = clamped;
    const LONG newWidth = static_cast<LONG>(mascotWidth_ * scale);
    const LONG newHeight = static_cast<LONG>(MascotHeightDip() * scale);
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
