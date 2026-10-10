#pragma once

#include <cstdint>

namespace cp::metrics {

inline constexpr float kTrayWidth = 330.0f;
inline constexpr float kPillHeight = 56.0f;
inline constexpr float kPillGap = 8.0f;
inline constexpr float kPeekOffset = 7.0f;
inline constexpr float kPeekInset = 10.0f;
inline constexpr int kMaxPeeks = 2;
inline constexpr int kMaxExpanded = 4;
inline constexpr float kTrayGap = 12.0f;
inline constexpr float kWindowMargin = 18.0f;
inline constexpr float kScreenMargin = 12.0f;
inline constexpr float kBadgeSize = 24.0f;
inline constexpr float kBadgeDockOffset = 4.0f;
inline constexpr int64_t kBadgeGraceMs = 700;
inline constexpr float kDefaultEdgeMargin = 24.0f;
inline constexpr float kSnapMargin = 16.0f;
inline constexpr float kBottomReserve = 8.0f;
inline constexpr float kEntranceOffset = 10.0f;
inline constexpr float kLagSlackX = 40.0f;
inline constexpr float kLagSlackY = 24.0f;
inline constexpr float kLagSoftZone = 28.0f;
inline constexpr float kLagRestEpsilon = 0.1f;
inline constexpr float kPillShadowSide = 5.0f;
inline constexpr float kPillShadowTop = 4.0f;
inline constexpr float kPillShadowBottom = 7.0f;
inline constexpr float kTrayFlipHysteresis = 24.0f;
inline constexpr float kMotionTauMs = 65.0f;
inline constexpr double kMaxFrameStepMs = 50.0;
inline constexpr int kDragThresholdPx = 4;
inline constexpr int kMinMascotWidth = 80;
inline constexpr int kMaxMascotWidth = 224;
inline constexpr uint32_t kAnimationTimer = 1;
inline constexpr uint32_t kCollapseTimer = 2;
inline constexpr uint32_t kCollapseDelayMs = 450;
inline constexpr int64_t kFrameWatchdogMs = 100;
inline constexpr int64_t kMinTimerDelayMs = 10;
inline constexpr int64_t kShimmerIntervalMs = 60;
inline constexpr int64_t kShimmerCycleMs = 2200;
inline constexpr int64_t kShimmerSweepMs = 1600;
inline constexpr int64_t kLookIdleMs = 2500;
inline constexpr float kSnapStiffness = 220.0f;
inline constexpr float kSnapDamping = 26.0f;

}
