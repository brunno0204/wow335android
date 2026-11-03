#include "M2AnimationPlayer.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace wow::android {
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
constexpr uint64_t unit = uint64_t(1) << 32;
// Linux host and the Android ARM64 toolchain both support unsigned 128-bit
// arithmetic. 64-bit milliseconds * bounded 32.32 speed fits in 107 bits.
using Wide = __uint128_t;
static_assert(sizeof(Wide) == 16);
} // namespace
uint64_t M2AnimationPlayer::encodeSpeed(double speed) {
    require(std::isfinite(speed) && speed >= 0 && speed <= 1024, "Invalid forward M2 playback speed");
    return uint64_t(std::llround(speed * double(unit)));
}
void M2AnimationPlayer::setPosition(Slot &slot, uint64_t milliseconds) {
    auto duration = slot.clip.duration();
    slot.clock.milliseconds =
        duration ? (slot.clock.loop ? milliseconds % duration : std::min<uint64_t>(milliseconds, duration))
                 : 0;
    slot.fraction = 0;
}
void M2AnimationPlayer::tick(Slot &slot, uint64_t milliseconds) {
    uint32_t duration = slot.clip.duration();
    if (!duration) {
        slot.clock.milliseconds = slot.fraction = 0;
        return;
    }
    const Wide phase =
        (Wide(slot.clock.milliseconds) << 32) + slot.fraction + Wide(milliseconds) * slot.speed;
    if (!slot.clock.loop && phase >= (Wide(duration) << 32)) {
        slot.clock.milliseconds = duration;
        slot.fraction = 0;
        return;
    }
    const Wide whole = phase >> 32;
    slot.clock.milliseconds = slot.clock.loop ? uint64_t(whole % duration) : uint64_t(whole);
    slot.fraction = uint32_t(phase);
}
void M2AnimationPlayer::play(const M2Animation &clip, const M2PlaybackOptions &options) {
    require(clip.initialized(), "Cannot play an uninitialized M2 animation");
    Slot next{clip, {0, options.loop}, encodeSpeed(options.speed), 0};
    setPosition(next, options.startMilliseconds);
    const uint32_t duration = options.transitionMilliseconds.value_or(clip.blendTime());
    std::optional<Slot> outgoing;
    uint32_t elapsed = 0, fade = 0;
    if (primary_ && duration) {
        // Follow SetPrimaryBoneSequence's dominant-secondary retention rule.
        // If the outgoing clip still dominates, keep its existing fade clock.
        if (outgoing_ && outgoingWeight() > .5f) {
            outgoing = outgoing_;
            elapsed = fadeElapsed_;
            fade = fadeDuration_;
        } else {
            outgoing = primary_;
            fade = duration;
        }
        // Validate compatibility/current sampling before committing any state.
        next.clip.evaluateBlended(outgoing->clip, next.clock, outgoing->clock, globalTime_, 1.0f);
    } else
        next.clip.evaluate(next.clock.milliseconds, globalTime_, next.clock.loop);
    primary_ = std::move(next);
    outgoing_ = std::move(outgoing);
    fadeElapsed_ = elapsed;
    fadeDuration_ = fade;
}
void M2AnimationPlayer::advance(uint64_t milliseconds) {
    require(milliseconds <= std::numeric_limits<uint64_t>::max() - globalTime_, "M2 global clock overflow");
    globalTime_ += milliseconds; // Pause affects clip/fade clocks, not global-loop time.
    if (paused_ || !primary_)
        return;
    tick(*primary_, milliseconds);
    if (outgoing_)
        tick(*outgoing_, milliseconds);
    if (outgoing_) {
        const uint32_t remaining = fadeDuration_ - fadeElapsed_;
        if (milliseconds >= remaining) {
            outgoing_.reset();
            fadeElapsed_ = fadeDuration_ = 0;
        } else
            fadeElapsed_ += uint32_t(milliseconds);
    }
}
void M2AnimationPlayer::stop() noexcept {
    primary_.reset();
    outgoing_.reset();
    fadeElapsed_ = fadeDuration_ = 0;
    paused_ = false;
}
void M2AnimationPlayer::seek(uint64_t milliseconds) {
    if (!primary_)
        throw std::logic_error("M2 player has no primary clip");
    setPosition(*primary_, milliseconds);
}
void M2AnimationPlayer::setSpeed(double speed) {
    if (!primary_)
        throw std::logic_error("M2 player has no primary clip");
    const auto encoded = encodeSpeed(speed);
    primary_->speed = encoded;
}
void M2AnimationPlayer::setLoop(bool loop) {
    if (!primary_)
        throw std::logic_error("M2 player has no primary clip");
    primary_->clock.loop = loop;
    auto duration = primary_->clip.duration();
    if (loop && duration)
        primary_->clock.milliseconds %= duration;
}
bool M2AnimationPlayer::finished() const noexcept {
    return primary_ && !primary_->clock.loop && primary_->clock.milliseconds >= primary_->clip.duration();
}
uint32_t M2AnimationPlayer::position() const noexcept {
    return primary_ ? uint32_t(primary_->clock.milliseconds) : 0;
}
double M2AnimationPlayer::speed() const noexcept {
    return primary_ ? double(primary_->speed) / double(unit) : 1;
}
float M2AnimationPlayer::outgoingWeight() const noexcept {
    if (!outgoing_ || !fadeDuration_)
        return 0;
    const float remaining = float(fadeDuration_ - fadeElapsed_) / float(fadeDuration_);
    return std::clamp(remaining * ((3.0f - 2.0f * remaining) * remaining), 0.0f, 1.0f);
}
std::vector<C44Matrix> M2AnimationPlayer::evaluate(const C44Matrix &world) const {
    if (!primary_)
        return {};
    if (outgoing_)
        return primary_->clip.evaluateBlended(outgoing_->clip, primary_->clock, outgoing_->clock, globalTime_,
                                              outgoingWeight(), world);
    return primary_->clip.evaluate(primary_->clock.milliseconds, globalTime_, primary_->clock.loop, world);
}
} // namespace wow::android
