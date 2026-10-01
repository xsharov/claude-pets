#include "animation.h"

#include <climits>
#include <cmath>

namespace cp {

namespace {

constexpr int kSlowIdleFactor = 6;
constexpr int kRepeatCount = 3;

std::vector<SpriteFrame> Strip(int row, int count, int durationMs, int lastDurationMs) {
    std::vector<SpriteFrame> frames;
    frames.reserve(static_cast<size_t>(count));
    for (int column = 0; column < count; ++column) {
        frames.push_back({row, column, column == count - 1 ? lastDurationMs : durationMs});
    }
    return frames;
}

std::vector<SpriteFrame> IdleFrames() {
    return {{0, 0, 280}, {0, 1, 110}, {0, 2, 110}, {0, 3, 140}, {0, 4, 140}, {0, 5, 320}};
}

std::vector<SpriteFrame> SlowIdleFrames() {
    std::vector<SpriteFrame> frames = IdleFrames();
    for (SpriteFrame& frame : frames) {
        frame.durationMs *= kSlowIdleFactor;
    }
    return frames;
}

std::vector<SpriteFrame> FramesFor(PetAnim anim) {
    switch (anim) {
    case PetAnim::Idle:
        return IdleFrames();
    case PetAnim::RunningRight:
        return Strip(1, 8, 120, 220);
    case PetAnim::RunningLeft:
        return Strip(2, 8, 120, 220);
    case PetAnim::Waving:
        return Strip(3, 4, 140, 280);
    case PetAnim::Jumping:
        return Strip(4, 5, 140, 280);
    case PetAnim::Failed:
        return Strip(5, 8, 140, 240);
    case PetAnim::Waiting:
        return Strip(6, 6, 150, 260);
    case PetAnim::Running:
        return Strip(7, 6, 120, 220);
    case PetAnim::Review:
        return Strip(8, 6, 150, 280);
    }
    return IdleFrames();
}

}

void SpriteAnimator::Build() {
    frames_.clear();
    index_ = 0;
    loopStart_ = 0;
    loops_ = true;
    if (reducedMotion_) {
        frames_.push_back(FramesFor(anim_).front());
        loops_ = false;
        return;
    }
    if (anim_ == PetAnim::Idle) {
        frames_ = SlowIdleFrames();
        return;
    }
    const std::vector<SpriteFrame> cycle = FramesFor(anim_);
    for (int i = 0; i < kRepeatCount; ++i) {
        frames_.insert(frames_.end(), cycle.begin(), cycle.end());
    }
    loopStart_ = frames_.size();
    const std::vector<SpriteFrame> idle = SlowIdleFrames();
    frames_.insert(frames_.end(), idle.begin(), idle.end());
}

void SpriteAnimator::Play(PetAnim anim, int64_t nowMs) {
    if (started_ && anim == anim_) {
        return;
    }
    anim_ = anim;
    Restart(nowMs);
}

void SpriteAnimator::Restart(int64_t nowMs) {
    started_ = true;
    Build();
    frameStartMs_ = nowMs;
}

void SpriteAnimator::SetReducedMotion(bool reduced, int64_t nowMs) {
    if (reduced == reducedMotion_) {
        return;
    }
    reducedMotion_ = reduced;
    Restart(nowMs);
}

bool SpriteAnimator::Tick(int64_t nowMs) {
    if (frames_.size() <= 1) {
        return false;
    }
    bool changed = false;
    int guard = 0;
    while (nowMs - frameStartMs_ >= frames_[index_].durationMs) {
        if (++guard > 64) {
            frameStartMs_ = nowMs;
            break;
        }
        size_t next = index_ + 1;
        if (next >= frames_.size()) {
            if (!loops_) {
                return changed;
            }
            next = loopStart_;
        }
        frameStartMs_ += frames_[index_].durationMs;
        index_ = next;
        changed = true;
    }
    return changed;
}

SpriteFrame SpriteAnimator::Frame() const {
    if (frames_.empty()) {
        return {};
    }
    return frames_[index_];
}

int64_t SpriteAnimator::NextDeadlineMs() const {
    if (frames_.size() <= 1) {
        return LLONG_MAX;
    }
    if (!loops_ && index_ + 1 >= frames_.size()) {
        return LLONG_MAX;
    }
    return frameStartMs_ + frames_[index_].durationMs;
}

std::optional<SpriteFrame> LookFrameToward(float dx, float dy) {
    if (std::hypot(dx, dy) <= 1.0f) {
        return std::nullopt;
    }
    constexpr double kPi = 3.14159265358979323846;
    double degrees = std::atan2(static_cast<double>(dx), static_cast<double>(-dy)) * 180.0 / kPi;
    degrees = std::fmod(degrees + 360.0, 360.0);
    const int index = static_cast<int>(std::lround(degrees / 22.5)) % 16;
    return SpriteFrame{9 + index / 8, index % 8, 0};
}

}
