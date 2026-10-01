#pragma once

#include <windows.h>

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
inline constexpr float kDefaultEdgeMargin = 24.0f;
inline constexpr float kSnapMargin = 16.0f;
inline constexpr float kBottomReserve = 8.0f;
inline constexpr float kEntranceOffset = 10.0f;
inline constexpr int kDragThresholdPx = 4;
inline constexpr int kMinMascotWidth = 80;
inline constexpr int kMaxMascotWidth = 224;
inline constexpr UINT_PTR kAnimationTimer = 1;
inline constexpr UINT_PTR kCollapseTimer = 2;
inline constexpr UINT kCollapseDelayMs = 450;
inline constexpr int64_t kShimmerIntervalMs = 60;
inline constexpr int64_t kShimmerCycleMs = 2200;
inline constexpr int64_t kShimmerSweepMs = 1600;
inline constexpr int64_t kLookIdleMs = 2500;
inline constexpr float kSnapStiffness = 220.0f;
inline constexpr float kSnapDamping = 26.0f;

}
