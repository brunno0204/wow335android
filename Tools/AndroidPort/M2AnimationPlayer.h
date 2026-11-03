#pragma once
#include "M2Animation.h"
#include <optional>
namespace wow::android {
struct M2PlaybackOptions {
    bool loop = true;
    double speed = 1;
    uint64_t startMilliseconds = 0;
    // Missing: use the requested sequence's declared blend time. Zero: hard cut.
    std::optional<uint32_t> transitionMilliseconds;
};
// Frame-thread-owned, forward-only whole-clip player. Immutable clips may be
// shared. It does not implement original per-bone sequence selection, random
// variations, callbacks/events, deferred IO or the complete CM2Model state machine.
class M2AnimationPlayer {
  public:
    // Every play restarts the requested clip; pause state is preserved.
    void play(const M2Animation &clip, const M2PlaybackOptions &options = {});
    void advance(uint64_t milliseconds);
    void pause(bool value) noexcept { paused_ = value; }
    void stop() noexcept;             // Clears clips/pause; preserves the independent global clock.
    void seek(uint64_t milliseconds); // Primary only; resets fractional phase.
    void setSpeed(double speed);      // Primary only. Quantized to unsigned 32.32, range [0,1024].
    void setLoop(bool loop);
    bool playing() const noexcept { return bool(primary_); }
    bool transitioning() const noexcept { return bool(outgoing_); }
    bool paused() const noexcept { return paused_; }
    bool finished() const noexcept;
    uint64_t globalTime() const noexcept { return globalTime_; }
    uint32_t position() const noexcept;
    double speed() const noexcept;
    float outgoingWeight() const noexcept;
    std::vector<C44Matrix> evaluate(const C44Matrix &world = C44Matrix(1)) const;

  private:
    struct Slot {
        M2Animation clip;
        M2AnimationClock clock;
        uint64_t speed = uint64_t(1) << 32;
        uint32_t fraction = 0;
    };
    std::optional<Slot> primary_, outgoing_;
    uint64_t globalTime_ = 0;
    uint32_t fadeElapsed_ = 0, fadeDuration_ = 0;
    bool paused_ = false;
    static uint64_t encodeSpeed(double speed);
    static void setPosition(Slot &slot, uint64_t milliseconds);
    static void tick(Slot &slot, uint64_t milliseconds);
};
} // namespace wow::android
