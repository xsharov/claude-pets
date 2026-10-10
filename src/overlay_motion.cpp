#include "overlay.h"

#include "log.h"
#include "overlay_metrics.h"
#include "util.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>

namespace cp {

using namespace metrics;

namespace {

float SoftShift(float value, float delta, float low, float high) {
    if (delta == 0.0f) {
        return value;
    }
    const bool forward = delta > 0.0f;
    const float room = forward ? high - value : value - low;
    if (room <= 0.0f) {
        return value;
    }
    const float distance = std::fabs(delta);
    const float free = std::max(0.0f, room - kLagSoftZone);
    if (distance <= free) {
        return value + delta;
    }
    const float remaining = std::min(room, kLagSoftZone) * std::exp(-(distance - free) / kLagSoftZone);
    return forward ? high - remaining : low + remaining;
}

float SettleLag(float value, float decay, float low, float high) {
    const float next = std::clamp(value * decay, low, high);
    return std::fabs(next) < kLagRestEpsilon ? 0.0f : next;
}

struct FrameStats {
    double windowStartMs = 0.0;
    double lastFrameMs = 0.0;
    double renderTotalMs = 0.0;
    double renderMaxMs = 0.0;
    double gapMaxMs = 0.0;
    float lagMaxX = 0.0f;
    float lagMaxY = 0.0f;
    int frames = 0;
    int clipped = 0;
};

FrameStats g_stats;

void FlushFrameStats(const FrameStats& stats, double endMs) {
    if (stats.frames < 2) {
        return;
    }
    const double span = std::max(1.0, endMs - stats.windowStartMs);
    char line[256];
    snprintf(line, sizeof(line), "frames %d in %.0f ms (%.1f fps) render avg %.2f max %.2f ms gap max %.2f ms lag max %.1f,%.1f clipped %d", stats.frames, span,
             (stats.frames - 1) * 1000.0 / span, stats.renderTotalMs / stats.frames, stats.renderMaxMs, stats.gapMaxMs, stats.lagMaxX, stats.lagMaxY,
             stats.clipped);
    LogLine(line);
}

}

void Overlay::RecordFrame(double now, double renderMs) {
    if (!LogEnabled()) {
        return;
    }
    FrameStats& stats = g_stats;
    if (stats.frames > 0 && now - stats.lastFrameMs > 250.0) {
        FlushFrameStats(stats, stats.lastFrameMs);
        stats = FrameStats{};
    }
    if (stats.frames == 0) {
        stats.windowStartMs = now;
    } else {
        stats.gapMaxMs = std::max(stats.gapMaxMs, now - stats.lastFrameMs);
    }
    stats.lastFrameMs = now;
    ++stats.frames;
    stats.renderTotalMs += renderMs;
    stats.renderMaxMs = std::max(stats.renderMaxMs, renderMs);
    stats.lagMaxX = std::max(stats.lagMaxX, std::fabs(lag_.x));
    stats.lagMaxY = std::max(stats.lagMaxY, std::fabs(lag_.y));
    const float width = static_cast<float>(layout_.window.right - layout_.window.left) / layout_.scale;
    const float height = static_cast<float>(layout_.window.bottom - layout_.window.top) / layout_.scale;
    for (const PillState& pill : pills_) {
        if (pill.opacity <= 0.01f) {
            continue;
        }
        const RectF rect = PillRect(pill);
        if (rect.left - kPillShadowSide < -0.5f || rect.right + kPillShadowSide > width + 0.5f || rect.top - kPillShadowTop < -0.5f ||
            rect.bottom + kPillShadowBottom > height + 0.5f) {
            ++stats.clipped;
            break;
        }
    }
    if (now - stats.windowStartMs >= 1000.0) {
        FlushFrameStats(stats, now);
        stats = FrameStats{};
    }
}

SpriteFrame Overlay::CurrentFrame() const {
    return lookFrame_.has_value() ? *lookFrame_ : animator_.Frame();
}

Overlay::LagRange Overlay::LagLimits() const {
    LagRange range;
    bool any = false;
    RectF bounds = {};
    for (const PillState& pill : pills_) {
        if (pill.opacity <= 0.01f && pill.targetOpacity <= 0.0f) {
            continue;
        }
        if (!any) {
            bounds = pill.rect;
            any = true;
            continue;
        }
        bounds.left = std::min(bounds.left, pill.rect.left);
        bounds.top = std::min(bounds.top, pill.rect.top);
        bounds.right = std::max(bounds.right, pill.rect.right);
        bounds.bottom = std::max(bounds.bottom, pill.rect.bottom);
    }
    if (!any || !layoutValid_) {
        return range;
    }
    const float width = static_cast<float>(layout_.window.right - layout_.window.left) / layout_.scale;
    const float height = static_cast<float>(layout_.window.bottom - layout_.window.top) / layout_.scale;
    const RectF& mascot = layout_.mascot;
    const RectF& content = renderer_.SpriteContentBounds();
    const float mascotHeight = mascot.bottom - mascot.top;
    range.minX = std::min(0.0f, kPillShadowSide - bounds.left);
    range.maxX = std::max(0.0f, width - kPillShadowSide - bounds.right);
    if (layout_.trayAbove) {
        range.minY = std::min(0.0f, kPillShadowTop - bounds.top);
        range.maxY = std::max(0.0f, mascot.top + content.top * mascotHeight - bounds.bottom);
    } else {
        range.minY = std::min(0.0f, mascot.top + content.bottom * mascotHeight - bounds.top);
        range.maxY = std::max(0.0f, height - kPillShadowBottom - bounds.bottom);
    }
    return range;
}

void Overlay::ShiftLag(float dx, float dy) {
    const LagRange range = LagLimits();
    lag_.x = SoftShift(lag_.x, dx, range.minX, range.maxX);
    lag_.y = SoftShift(lag_.y, dy, range.minY, range.maxY);
}

bool Overlay::StepAnimations(double now, bool& relayout) {
    const float elapsed = static_cast<float>(std::clamp(now - lastAnimationMs_, 0.0, kMaxFrameStepMs));
    lastAnimationMs_ = now;
    const float k = 1.0f - std::exp(-elapsed / kMotionTauMs);
    bool animating = false;
    auto approach = [&](float& value, float target, float epsilon) {
        const float delta = target - value;
        if (std::fabs(delta) <= epsilon) {
            value = target;
            return;
        }
        value += delta * k;
        animating = true;
    };
    bool fadedOut = false;
    for (PillState& pill : pills_) {
        approach(pill.rect.left, pill.target.left, 0.2f);
        approach(pill.rect.top, pill.target.top, 0.2f);
        approach(pill.rect.right, pill.target.right, 0.2f);
        approach(pill.rect.bottom, pill.target.bottom, 0.2f);
        const bool wasVisible = pill.opacity > 0.01f;
        approach(pill.opacity, pill.targetOpacity, 0.01f);
        fadedOut = fadedOut || (wasVisible && pill.opacity <= 0.01f);
    }
    if (lag_.x != 0.0f || lag_.y != 0.0f) {
        animating = true;
        const LagRange range = LagLimits();
        lag_.x = SettleLag(lag_.x, 1.0f - k, range.minX, range.maxX);
        lag_.y = SettleLag(lag_.y, 1.0f - k, range.minY, range.maxY);
    }
    const size_t before = pills_.size();
    pills_.erase(std::remove_if(pills_.begin(), pills_.end(), [](const PillState& pill) { return pill.leaving && pill.opacity <= 0.01f; }), pills_.end());
    relayout = fadedOut || pills_.size() != before;
    return animating;
}

bool Overlay::Animating() const {
    if (dragging_ || snapping_ || lag_.x != 0.0f || lag_.y != 0.0f) {
        return true;
    }
    for (const PillState& pill : pills_) {
        if (pill.opacity != pill.targetOpacity || pill.rect.left != pill.target.left || pill.rect.top != pill.target.top ||
            pill.rect.right != pill.target.right || pill.rect.bottom != pill.target.bottom) {
            return true;
        }
    }
    return false;
}

void Overlay::Advance(double now) {
    if (!shown_) {
        window_.KillTimer(kAnimationTimer);
        window_.SetFrameClockActive(false);
        return;
    }
    const int64_t ticks = static_cast<int64_t>(now);
    LayoutChange change = LayoutChange::None;
    auto merge = [&change](LayoutChange next) {
        if (static_cast<int>(next) > static_cast<int>(change)) {
            change = next;
        }
    };
    if (dragging_) {
        merge(StepDrag(ticks));
    }
    if (snapping_) {
        merge(StepSnap(now));
    }
    bool relayout = false;
    const bool animating = StepAnimations(now, relayout);
    const bool badgeExpired = badgeHoldUntilMs_ != 0 && ticks >= badgeHoldUntilMs_;
    if (badgeExpired) {
        badgeHoldUntilMs_ = 0;
    }
    if (relayout) {
        Relayout();
    }
    FinishPressIfReleased();
    PollHover();
    animator_.Tick(ticks);
    UpdateLook(ticks);
    bool partial = !(CurrentFrame() == drawnFrame_);
    if (HasVisibleLoading() && ticks - lastShimmerMs_ >= kShimmerIntervalMs) {
        lastShimmerMs_ = ticks;
        partial = true;
    }
    const bool full = animating || relayout || badgeExpired || change == LayoutChange::Geometry;
    if (full || partial) {
        const double renderStart = MonotonicMs();
        Render(full);
        RecordFrame(now, MonotonicMs() - renderStart);
    } else if (change == LayoutChange::Moved) {
        window_.Move(PointI{layout_.window.left, layout_.window.top});
    }
    ScheduleTimer();
}

void Overlay::ScheduleTimer() {
    if (!window_.Created()) {
        return;
    }
    if (!shown_) {
        window_.KillTimer(kAnimationTimer);
        window_.SetFrameClockActive(false);
        return;
    }
    const int64_t now = static_cast<int64_t>(MonotonicMs());
    int64_t next = LLONG_MAX;
    if (Animating()) {
        if (!window_.FrameClockActive()) {
            lastAnimationMs_ = MonotonicMs();
            window_.SetFrameClockActive(true);
        }
        if (window_.FrameClockRunning()) {
            window_.SetTimer(kAnimationTimer, static_cast<uint32_t>(kFrameWatchdogMs));
            return;
        }
        next = now + 16;
    } else {
        window_.SetFrameClockActive(false);
    }
    if (!lookFrame_.has_value()) {
        next = std::min(next, animator_.NextDeadlineMs());
    }
    if (HasVisibleLoading()) {
        next = std::min(next, lastShimmerMs_ + kShimmerIntervalMs);
    }
    if (LookEligible()) {
        next = std::min(next, std::max(nextLookPollMs_, now + 10));
    }
    if (lookFrame_.has_value() && !LookEligible()) {
        next = std::min(next, now + 10);
    }
    if (hover_.part != HitPart::None || hoverMascot_ || expanded_) {
        next = std::min(next, now + 100);
    }
    if (badgeHoldUntilMs_ != 0) {
        next = std::min(next, badgeHoldUntilMs_);
    }
    if (pressed_) {
        next = std::min(next, now + 50);
    }
    if (next == LLONG_MAX) {
        window_.KillTimer(kAnimationTimer);
        return;
    }
    const int64_t delay = std::clamp<int64_t>(next - now, kMinTimerDelayMs, 60000);
    window_.SetTimer(kAnimationTimer, static_cast<uint32_t>(delay));
}

PointI Overlay::DragAnchor(PointI cursor) const {
    const float scale = layout_.scale;
    const PointI anchor = {cursor.x - static_cast<int32_t>(std::lround(grabFractionX_ * mascotWidth_ * scale)),
                           cursor.y - static_cast<int32_t>(std::lround(grabFractionY_ * MascotHeightDip() * scale))};
    return ClampAnchor(anchor, DisplayFromPoint(cursor).movable, scale);
}

Overlay::LayoutChange Overlay::StepDrag(int64_t now) {
    const PointI cursor = CursorPosition();
    const int32_t step = cursor.x - lastDragX_;
    if (step >= kDragThresholdPx) {
        transientAnim_ = PetAnim::RunningRight;
        lastDragX_ = cursor.x;
    } else if (step <= -kDragThresholdPx) {
        transientAnim_ = PetAnim::RunningLeft;
        lastDragX_ = cursor.x;
    }
    RefreshAnim(now);
    return MoveAnchor(DragAnchor(cursor));
}

Overlay::LayoutChange Overlay::MoveAnchor(PointI anchor) {
    if (anchor.x == anchor_.x && anchor.y == anchor_.y) {
        return LayoutChange::None;
    }
    anchor_ = anchor;
    return Relayout();
}

Overlay::LayoutChange Overlay::StepSnap(double now) {
    if (!snapping_) {
        return LayoutChange::None;
    }
    const float dt = static_cast<float>(std::clamp(now - snapLastMs_, 0.0, 32.0)) / 1000.0f;
    snapLastMs_ = now;
    const float ax = kSnapStiffness * (snapTarget_.x - snapX_) - kSnapDamping * snapVx_;
    const float ay = kSnapStiffness * (snapTarget_.y - snapY_) - kSnapDamping * snapVy_;
    snapVx_ += ax * dt;
    snapVy_ += ay * dt;
    snapX_ += snapVx_ * dt;
    snapY_ += snapVy_ * dt;
    const float distance = std::hypot(snapTarget_.x - snapX_, snapTarget_.y - snapY_);
    const float speed = std::hypot(snapVx_, snapVy_);
    if ((distance <= 0.5f && speed < 10.0f) || now - snapStartMs_ >= 1000.0) {
        snapping_ = false;
        const LayoutChange change = MoveAnchor(snapTarget_);
        PersistPosition();
        return change;
    }
    return MoveAnchor(PointI{static_cast<int32_t>(std::lround(snapX_)), static_cast<int32_t>(std::lround(snapY_))});
}

}
