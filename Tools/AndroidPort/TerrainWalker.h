#pragma once
#include "Engine/Source/Event/Types.h"
#include "TerrainSurface.h"
namespace wow::android {
struct TerrainProbePosition {
    double x = 0, y = 0, z = 0;
};
// Development ground point, not the original character movement/physics system.
class TerrainWalker {
  public:
    explicit TerrainWalker(std::shared_ptr<const TerrainSurface> surface);
    bool place(double x, double y);
    // Development ballistic jump: speed10, gravity20; only from valid ground.
    bool jump() noexcept;
    // Frame owner: 0..100ms, unit horizontal world direction. Substeps <=.25 units.
    void advance(uint32_t milliseconds, double directionX, double directionY);
    bool placed() const noexcept { return placed_; }
    bool blocked() const noexcept { return blocked_; }
    bool onGround() const noexcept { return placed_ && grounded_; }
    double verticalSpeed() const noexcept { return verticalSpeed_; }
    const TerrainProbePosition &position() const noexcept { return position_; }

  private:
    std::shared_ptr<const TerrainSurface> surface_;
    TerrainProbePosition position_;
    double groundHeight_ = 0, verticalSpeed_ = 0;
    bool placed_ = false, blocked_ = false, grounded_ = false;
};
// Frame-owned W/S/Q/E state and Space press latch from original touch Events.
class TerrainProbeInput {
  public:
    void key(KEY key, bool down) noexcept;
    void clear() noexcept { forward_ = back_ = left_ = right_ = jumpHeld_ = jumpPressed_ = false; }
    bool consumeJump() noexcept {
        const bool pressed = jumpPressed_;
        jumpPressed_ = false;
        return pressed;
    }
    double right() const noexcept { return int(right_) - int(left_); }
    double forward() const noexcept { return int(forward_) - int(back_); }

  private:
    bool forward_ = false, back_ = false, left_ = false, right_ = false;
    bool jumpHeld_ = false, jumpPressed_ = false;
};
} // namespace wow::android
