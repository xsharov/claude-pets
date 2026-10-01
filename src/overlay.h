#pragma once

#include "animation.h"
#include "notices.h"
#include "renderer.h"
#include "settings.h"

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cp {

struct OverlayCallbacks {
    std::function<void(const Notice&)> openNotice;
    std::function<void(const Notice&)> dismissNotice;
    std::function<void(const Notice&, POINT)> noticeMenu;
    std::function<void(POINT)> contextMenu;
    std::function<void()> mascotClicked;
    std::function<void(bool)> activityVisibilityRequested;
    std::function<void(const std::wstring&, POINT)> positionChanged;
    std::function<bool(UINT, WPARAM, LPARAM, LRESULT&)> messageHandler;
};

class Overlay {
public:
    Overlay() = default;
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool Create(HINSTANCE instance, OverlayCallbacks callbacks);
    void Destroy();
    HWND Handle() const { return window_; }
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
    POINT Anchor() const { return anchor_; }

private:
    enum class HitPart : uint8_t { None, Mascot, Badge, Pill, Peek, Dismiss };

    struct Hit {
        HitPart part = HitPart::None;
        int pill = -1;
        bool operator==(const Hit&) const = default;
    };

    struct Layout {
        float scale = 1.0f;
        RECT work = {};
        RECT window = {};
        D2D1_RECT_F mascot = {};
        D2D1_RECT_F badge = {};
        float trayLeft = 0.0f;
        float trayWidth = 0.0f;
        float trayNearEdge = 0.0f;
        bool trayAbove = true;
    };

    struct PillState {
        Notice notice;
        D2D1_RECT_F rect = {};
        D2D1_RECT_F target = {};
        float opacity = 0.0f;
        float targetOpacity = 0.0f;
        int depth = 0;
        bool contentVisible = true;
        bool leaving = false;
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    Layout ComputeLayout() const;
    void ApplyLayout(bool forceRender);
    void UpdatePillTargets();
    void SyncPills();
    void PreserveOrderWhileHovered(std::vector<Notice>& notices) const;
    bool StepAnimations(int64_t now);
    float MascotHeightDip() const;
    int VisibleNoticeCount() const;
    int ExpandedCapacity() const;
    bool Expanded() const;
    void SetExpanded(bool expanded);

    void Render(bool full);
    void ScheduleTimer();
    void OnTimer(int64_t now);
    PetAnim EffectiveAnim() const;
    void RefreshAnim(int64_t now);
    void UpdateLook(int64_t now);
    bool LookEligible() const;
    std::optional<D2D1_RECT_F> BadgeRect() const;
    bool BadgeVisible() const;
    float ShimmerPhase(int64_t now) const;
    bool HasVisibleLoading() const;
    RECT DipToWindowPx(const D2D1_RECT_F& rect) const;

    Hit HitTest(POINT clientPx) const;
    void OnMouseMove(POINT clientPx);
    void OnMouseLeave();
    void OnLeftDown(POINT clientPx);
    void OnLeftUp(POINT clientPx);
    void OnRightUp(POINT clientPx);
    void OnWheel(int delta);
    void OnCaptureLost();
    bool OnSetCursor();
    void UpdateHover(const Hit& hit);
    void PollHover();
    void MoveAnchor(POINT anchor);
    POINT ClampAnchor(POINT anchor, const RECT& work, float scale) const;
    void StartSnap(POINT cursor);
    bool StepSnap(int64_t now);
    void PersistPosition();
    void ActivateBadge();

    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    OverlayCallbacks callbacks_;
    Renderer renderer_;
    Layout layout_;
    bool layoutValid_ = false;
    bool frameValid_ = false;
    bool shown_ = false;

    POINT anchor_ = {};
    int mascotWidth_ = 112;
    std::vector<Notice> notices_;
    std::vector<PillState> pills_;
    PetAnim baseAnim_ = PetAnim::Idle;
    std::optional<PetAnim> transientAnim_;
    SpriteAnimator animator_;
    SpriteFrame drawnFrame_ = {-1, -1, 0};
    bool activityVisible_ = true;
    bool expanded_ = false;
    int scrollOffset_ = 0;
    bool lookAtCursor_ = true;
    std::optional<SpriteFrame> lookFrame_;
    POINT lastCursor_ = {LONG_MIN, LONG_MIN};
    int64_t lastCursorMoveMs_ = 0;
    int64_t nextLookPollMs_ = 0;
    int64_t shimmerEpochMs_ = 0;
    int64_t lastShimmerMs_ = 0;
    int64_t lastAnimationMs_ = 0;

    Hit hover_;
    bool hoverMascot_ = false;
    bool trackingLeave_ = false;
    bool collapsePending_ = false;

    bool pressed_ = false;
    bool dragging_ = false;
    Hit pressHit_;
    POINT pressCursor_ = {};
    float grabFractionX_ = 0.5f;
    float grabFractionY_ = 0.5f;
    LONG lastDragX_ = 0;

    bool snapping_ = false;
    float snapX_ = 0.0f;
    float snapY_ = 0.0f;
    float snapVx_ = 0.0f;
    float snapVy_ = 0.0f;
    POINT snapTarget_ = {};
    int64_t snapStartMs_ = 0;
    int64_t snapLastMs_ = 0;
};

}
