#pragma once

#include <cstdint>

namespace cp {

struct PointI {
    int32_t x = 0;
    int32_t y = 0;
    bool operator==(const PointI&) const = default;
};

struct RectI {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
    int32_t Width() const { return right - left; }
    int32_t Height() const { return bottom - top; }
    bool operator==(const RectI&) const = default;
};

struct PointF {
    float x = 0.0f;
    float y = 0.0f;
};

struct RectF {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

}
