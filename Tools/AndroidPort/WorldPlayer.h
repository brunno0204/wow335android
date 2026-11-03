#pragma once
#include "Engine/Source/Tempest/C44Matrix.h"
#include "WorldCollision.h"
#include <optional>
namespace wow::android {
// GL-thread local mover and chase camera for the in-world view. Development controller
// over WorldCollision (terrain + WMO/M2 collision meshes): step-up, walls with slide,
// falling and jumping. Not the original CMovement_C.
class WorldPlayer {
  public:
    // Places the mover on the highest walkable surface at or below z + 2 (WMO floors,
    // caves) or, failing that, any surface below. Returns success.
    bool place(const WorldCollision &world, double x, double y, double z);
    // A late-arriving floor (WMO loaded after the terrain) lifts an unmoved player.
    void refloor(const WorldCollision &world);
    // right/forward: analog stick (-1..1). A small push walks; pulling straight back walks
    // backwards facing ahead (original backpedal) instead of turning around.
    void frame(const WorldCollision &world, uint64_t milliseconds, double right, double forward,
               bool jump, bool walk = false);
    void orbit(float dx, float dy);
    // Camera-only rotation: while held, moving keeps the course it had (the camera swings
    // freely); released, the camera eases back behind the direction of travel.
    void freeLook(bool held);
    // Liquid surface height at the player (nullopt: no liquid); deep enough water swims.
    void setWater(std::optional<double> surface) { water_ = surface; }
    bool swimming() const noexcept { return swimming_; }
    bool walking() const noexcept { return walking_; }
    bool backward() const noexcept { return backward_; }
    double speed() const noexcept { return speed_; }
    void zoom(float factor); // pinch: >1 zooms in
    void faceCamera(double facing);
    void setRunSpeed(float speed) { runSpeed_ = speed > 0 && speed < 100 ? speed : 7; }
    void resetFrameClock() noexcept { lastFrame_.reset(); }
    C44Matrix view() const;
    C44Matrix projection(float aspect) const;
    bool placed() const noexcept { return placed_; }
    bool blocked() const noexcept { return blocked_; }
    bool onGround() const noexcept { return placed_ && grounded_; }
    double verticalSpeed() const noexcept { return verticalSpeed_; }
    double x() const noexcept { return x_; }
    double y() const noexcept { return y_; }
    double z() const noexcept { return z_; }
    double moveX() const noexcept { return moveX_; }
    double moveY() const noexcept { return moveY_; }
    double heading() const noexcept { return heading_; }
    const C3Vector &eye() const noexcept { return eye_; }
    bool moved() const noexcept { return moved_; }

  private:
    void step(const WorldCollision &world, double dx, double dy, double dt);
    void updateEye(const WorldCollision &world);
    double x_ = 0, y_ = 0, z_ = 0, placeZ_ = 0, verticalSpeed_ = 0;
    double moveX_ = 0, moveY_ = 0, heading_ = 0;
    double yaw_ = 0, pitch_ = .45, distance_ = 14;
    double lockYaw_ = 0, speed_ = 0;
    bool freeLook_ = false, locked_ = false, walking_ = false, backward_ = false, swimming_ = false;
    std::optional<double> water_;
    float runSpeed_ = 7;
    bool placed_ = false, grounded_ = false, blocked_ = false, moved_ = false;
    C3Vector eye_{}, target_{};
    std::optional<uint64_t> lastFrame_;
};
} // namespace wow::android
