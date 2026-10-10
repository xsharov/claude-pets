#include "overlay.h"

#include "log.h"
#include "overlay_metrics.h"
#include "util.h"

#include <algorithm>
#include <cmath>

namespace cp {

using namespace metrics;

namespace {

int64_t Ticks() {
    return static_cast<int64_t>(MonotonicMs());
}

RectI UnionRect(const RectI& a, const RectI& b) {
    if (a.right <= a.left || a.bottom <= a.top) {
        return b;
    }
    if (b.right <= b.left || b.bottom <= b.top) {
        return a;
    }
    return RectI{std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

}

Overlay::~Overlay() {
    Destroy();
}

bool Overlay::Create(OverlayCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
    if (!renderer_.Initialize()) {
        return false;
    }
    if (!window_.Create(*this, renderer_)) {
        return false;
    }
    shimmerEpochMs_ = Ticks();
    lastAnimationMs_ = MonotonicMs();
    animator_.SetReducedMotion(ReducedMotionEnabled(), shimmerEpochMs_);
    animator_.Play(PetAnim::Idle, shimmerEpochMs_);
    return true;
}

void Overlay::Destroy() {
    window_.Destroy();
}

void Overlay::OnFrame() {
    Advance(MonotonicMs());
}

void Overlay::OnTimer(uint32_t id) {
    if (id == kAnimationTimer) {
        Advance(MonotonicMs());
        return;
    }
    if (id == kCollapseTimer) {
        LogLine("collapse timer hover=" + std::to_string(static_cast<int>(hover_.part)));
        window_.KillTimer(kCollapseTimer);
        collapsePending_ = false;
        if (!(hover_.part == HitPart::Pill || hover_.part == HitPart::Peek || hover_.part == HitPart::Dismiss || hover_.part == HitPart::Badge)) {
            SetExpanded(false);
        }
    }
}

void Overlay::OnDisplayChanged() {
    HandleDisplayChange();
}

void Overlay::OnReducedMotionChanged() {
    animator_.SetReducedMotion(ReducedMotionEnabled(), Ticks());
    Render(false);
    ScheduleTimer();
}

void Overlay::Show() {
    if (!window_.Created()) {
        return;
    }
    shown_ = true;
    frameValid_ = false;
    ApplyLayout(true);
    window_.Show();
    ScheduleTimer();
}

void Overlay::Hide() {
    if (!window_.Created()) {
        return;
    }
    shown_ = false;
    window_.KillTimer(kAnimationTimer);
    window_.SetFrameClockActive(false);
    window_.Hide();
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
    const PointI cursor = CursorPosition();
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
    if (!window_.Created()) {
        return;
    }
    if (!layoutValid_) {
        ApplyLayout(true);
        return;
    }
    RectI partial = {};
    const RectI* region = nullptr;
    if (!full && frameValid_) {
        RectF mascot = layout_.mascot;
        mascot.left -= 2.0f;
        mascot.top -= 2.0f;
        mascot.right += 2.0f;
        mascot.bottom += 2.0f;
        partial = DipToWindowPx(mascot);
        if (BadgeVisible()) {
            partial = UnionRect(partial, DipToWindowPx(BadgeArea()));
        }
        for (const PillState& pill : pills_) {
            if (pill.notice.loading && pill.contentVisible && pill.opacity > 0.05f) {
                const RectF rect = PillRect(pill);
                const RectF line = {rect.left + 12.0f, rect.top + 26.0f, rect.right - 12.0f, rect.top + 46.0f};
                partial = UnionRect(partial, DipToWindowPx(line));
            }
        }
        if (partial.right > partial.left && partial.bottom > partial.top) {
            region = &partial;
        }
    }
    if (window_.Present(layout_.window, layout_.scale, region)) {
        frameValid_ = true;
        drawnFrame_ = paintedFrame_;
    } else {
        frameValid_ = false;
    }
}

void Overlay::Paint() {
    const int64_t now = Ticks();
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
        badge.rect = BadgeArea();
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
    paintedFrame_ = frame;
}

}
