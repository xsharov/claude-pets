#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace cp {

enum class PetAnim : uint8_t {
    Idle,
    RunningRight,
    RunningLeft,
    Waving,
    Jumping,
    Failed,
    Waiting,
    Running,
    Review,
};

struct SpriteFrame {
    int row = 0;
    int column = 0;
    int durationMs = 0;
    bool operator==(const SpriteFrame&) const = default;
};

constexpr int kSpriteColumns = 8;
constexpr int kSpriteRowsV1 = 9;
constexpr int kSpriteRowsV2 = 11;
constexpr float kCellAspect = 192.0f / 208.0f;

class SpriteAnimator {
public:
    void Play(PetAnim anim, int64_t nowMs);
    void Restart(int64_t nowMs);
    bool Tick(int64_t nowMs);
    SpriteFrame Frame() const;
    int64_t NextDeadlineMs() const;
    PetAnim Anim() const { return anim_; }
    void SetReducedMotion(bool reduced, int64_t nowMs);

private:
    void Build();

    PetAnim anim_ = PetAnim::Idle;
    bool started_ = false;
    bool reducedMotion_ = false;
    std::vector<SpriteFrame> frames_;
    size_t loopStart_ = 0;
    bool loops_ = true;
    size_t index_ = 0;
    int64_t frameStartMs_ = 0;
};

std::optional<SpriteFrame> LookFrameToward(float dx, float dy);

}
