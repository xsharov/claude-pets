#pragma once

#include "animation.h"
#include "geometry.h"
#include "notices.h"
#include "overlay_window.h"
#include "renderer.h"
#include "settings.h"

#include <climits>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cp {

struct OverlayCallbacks {
    std::function<void(const Notice&)> openNotice;
    std::function<void(const Notice&)> dismissNotice;
    std::function<void(const Notice&, PointI)> noticeMenu;
    std::function<void(PointI)> contextMenu;
    std::function<void()> mascotClicked;
    std::function<void(bool)> activityVisibilityRequested;
    std::function<void(const std::wstring&, PointI)> positionChanged;
};

class Overlay final : private OverlayWindowEvents {
public:
    Overlay() = default;
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool Create(OverlayCallbacks callbacks);
    void Destroy();
    OverlayWindow& Window() { return window_; }
    Renderer& Graphics() { return renderer_; }

    void PlaceInitial(const Settings& settings);
    void SetMascotWidth(int widthDip);
    void SetNotices(std::vector<Notice> notices, PetAnim mascotAnim);
    void SetActivityVisible(bool visible);
    void SetLookAtCursor(bool enabled);
    void SetDarkTheme(bool dark);
    void PetChanged();
    void Show();
    void Hide();
    bool IsShown() const { return shown_; }
    void HandleDisplayChange();
    std::wstring MonitorKey() const;
    PointI Anchor() const { return anchor_; }

private:
    enum class HitPart : uint8_t { None, Mascot, Badge, Pill, Peek, Dismiss };
    enum class LayoutChange : uint8_t { None, Moved, Geometry };

    struct Hit {
        HitPart part = HitPart::None;
        int pill = -1;
        bool operator==(const Hit&) const = default;
    };

    struct LagRange {
        float minX = 0.0f;
        float maxX = 0.0f;
        float minY = 0.0f;
        float maxY = 0.0f;
    };

    struct Layout {
        DisplayId monitor = 0;
        float scale = 1.0f;
        RectI work = {};
        RectI movable = {};
        RectI window = {};
        RectF mascot = {};
        float trayLeft = 0.0f;
        float trayWidth = 0.0f;
        float trayNearEdge = 0.0f;
        bool trayAbove = true;
    };

    struct PillState {
        Notice notice;
        RectF rect = {};
        RectF target = {};
        float opacity = 0.0f;
        float targetOpacity = 0.0f;
        int depth = 0;
        bool contentVisible = true;
        bool leaving = false;
    };

    void OnPointerMove(PointI client, bool primaryDown) override;
    void OnPointerLeave() override;
    void OnPrimaryDown(PointI client) override;
    void OnPrimaryUp(PointI client) override;
    void OnSecondaryUp(PointI client) override;
    void OnWheel(int delta) override;
    void OnCaptureLost() override;
    CursorShape CursorAt(PointI client) override;
    bool Interactive(PointI client) override;
    void OnFrame() override;
    void OnTimer(uint32_t id) override;
    void OnDisplayChanged() override;
    void OnReducedMotionChanged() override;
    void Paint() override;

    Layout ComputeLayout() const;
    LayoutChange Relayout();
    void ApplyLayout(bool forceRender);
    void UpdatePillTargets();
    void SyncPills();
    void PreserveOrderWhileHovered(std::vector<Notice>& notices) const;
    float MascotHeightDip() const;
    int VisibleNoticeCount() const;
    int ExpandedCapacity() const;
    bool Expanded() const;
    void SetExpanded(bool expanded);

    void Render(bool full);
    void ScheduleTimer();
    void Advance(double now);
    void RecordFrame(double now, double renderMs);
    bool Animating() const;
    bool StepAnimations(double now, bool& relayout);
    bool PillsVisible() const;
    RectF PillRect(const PillState& pill) const;
    LagRange LagLimits() const;
    void ShiftLag(float dx, float dy);
    SpriteFrame CurrentFrame() const;
    PetAnim EffectiveAnim() const;
    void RefreshAnim(int64_t now);
    void UpdateLook(int64_t now);
    bool LookEligible() const;
    std::optional<RectF> BadgeRect() const;
    RectF BadgeArea() const;
    bool BadgeVisible() const;
    bool BadgeWanted() const;
    float ShimmerPhase(int64_t now) const;
    bool HasVisibleLoading() const;
    RectI DipToWindowPx(const RectF& rect) const;
    RectF MascotHitRect() const;

    Hit HitTest(PointI clientPx) const;
    void FinishPressIfReleased();
    void UpdateHover(const Hit& hit);
    void PollHover();
    LayoutChange MoveAnchor(PointI anchor);
    PointI ClampAnchor(PointI anchor, const RectI& area, float scale) const;
    PointI DragAnchor(PointI cursor) const;
    LayoutChange StepDrag(int64_t now);
    void StartSnap(PointI cursor);
    LayoutChange StepSnap(double now);
    void PersistPosition();
    void ActivateBadge();

    OverlayCallbacks callbacks_;
    Renderer renderer_;
    OverlayWindow window_;
    Layout layout_;
    bool layoutValid_ = false;
    bool frameValid_ = false;
    bool shown_ = false;

    PointI anchor_ = {};
    int mascotWidth_ = 112;
    std::vector<Notice> notices_;
    std::vector<PillState> pills_;
    PetAnim baseAnim_ = PetAnim::Idle;
    std::optional<PetAnim> transientAnim_;
    SpriteAnimator animator_;
    SpriteFrame drawnFrame_ = {-1, -1, 0};
    SpriteFrame paintedFrame_ = {-1, -1, 0};
    bool activityVisible_ = true;
    bool expanded_ = false;
    int scrollOffset_ = 0;
    bool lookAtCursor_ = true;
    std::optional<SpriteFrame> lookFrame_;
    PointI lastCursor_ = {INT32_MIN, INT32_MIN};
    int64_t lastCursorMoveMs_ = 0;
    int64_t nextLookPollMs_ = 0;
    int64_t shimmerEpochMs_ = 0;
    int64_t lastShimmerMs_ = 0;
    double lastAnimationMs_ = 0.0;
    PointF lag_ = {};

    Hit hover_;
    bool hoverMascot_ = false;
    int64_t badgeHoldUntilMs_ = 0;
    bool collapsePending_ = false;

    bool pressed_ = false;
    bool dragging_ = false;
    Hit pressHit_;
    PointI pressCursor_ = {};
    float grabFractionX_ = 0.5f;
    float grabFractionY_ = 0.5f;
    int32_t lastDragX_ = 0;

    bool snapping_ = false;
    float snapX_ = 0.0f;
    float snapY_ = 0.0f;
    float snapVx_ = 0.0f;
    float snapVy_ = 0.0f;
    PointI snapTarget_ = {};
    double snapStartMs_ = 0.0;
    double snapLastMs_ = 0.0;
};

}
