#include "WorldPlayer.h"
#include <algorithm>
#include <cmath>
namespace wow::android {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double gravity = 19.2911, jumpSpeed = 7.9555, headHeight = 1.9;
// Swimming: the body floats with the feet this far under the surface (chest-deep water).
constexpr double swimDepth = 1.45, swimSpeed = 4.7222;
} // namespace
bool WorldPlayer::place(const WorldCollision &world, double x, double y, double z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;
    auto ground = world.groundBelow(x, y, z + 2);
    if (!ground)
        ground = world.groundBelow(x, y, z + 600, .0);
    if (!ground)
        return false;
    x_ = x;
    y_ = y;
    z_ = ground->z;
    placeZ_ = z;
    verticalSpeed_ = 0;
    grounded_ = placed_ = true;
    blocked_ = moved_ = false;
    updateEye(world);
    return true;
}
void WorldPlayer::refloor(const WorldCollision &world) {
    if (!placed_ || moved_)
        return;
    if (auto ground = world.groundBelow(x_, y_, placeZ_ + 2); ground && ground->z > z_ + 1e-3) {
        z_ = ground->z;
        verticalSpeed_ = 0;
        grounded_ = true;
    }
}
void WorldPlayer::faceCamera(double facing) {
    if (std::isfinite(facing)) {
        yaw_ = std::remainder(pi / 2 - facing, 2 * pi);
        heading_ = facing;
    }
}
void WorldPlayer::orbit(float dx, float dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy))
        return;
    yaw_ = std::remainder(yaw_ + double(dx) * .005, 2 * pi);
    pitch_ = std::clamp(pitch_ + double(dy) * .005, -1.2, 1.45);
}
void WorldPlayer::zoom(float factor) {
    if (std::isfinite(factor) && factor > 0)
        distance_ = std::clamp(distance_ / double(factor), 1.5, 50.0);
}
void WorldPlayer::freeLook(bool held) {
    if (held && !freeLook_) {
        lockYaw_ = locked_ ? lockYaw_ : yaw_;
        locked_ = true;
    }
    freeLook_ = held;
}
void WorldPlayer::frame(const WorldCollision &world, uint64_t milliseconds, double right,
                        double forward, bool jump, bool walk) {
    const auto delta = lastFrame_ && milliseconds >= *lastFrame_
                           ? std::min<uint64_t>(milliseconds - *lastFrame_, 100)
                           : 0;
    lastFrame_ = milliseconds;
    blocked_ = false;
    if (!placed_)
        return;
    if (!std::isfinite(right) || !std::isfinite(forward))
        right = forward = 0;
    const double magnitude = std::hypot(right, forward);
    if (magnitude > 1) {
        right /= magnitude;
        forward /= magnitude;
    }
    const bool moving = magnitude > 1e-3;
    // Free look: the course is the locked yaw; after release the camera eases back.
    if (locked_ && !freeLook_) {
        const double gap = std::remainder(lockYaw_ - yaw_, 2 * pi);
        if (!moving || std::abs(gap) < .02) {
            if (moving)
                yaw_ = lockYaw_;
            locked_ = false;
        } else if (delta)
            yaw_ = std::remainder(yaw_ + gap * std::min(1.0, delta * .006), 2 * pi);
    }
    if (locked_ && freeLook_ && !moving)
        lockYaw_ = yaw_; // standing still: free look is just the camera
    const double basis = locked_ ? lockYaw_ : yaw_;
    const double c = std::cos(basis), s = std::sin(basis);
    backward_ = moving && forward < -.5 && std::abs(right) < std::abs(forward) * .6;
    if (moving) // the mode sticks while standing (no run/walk toggles when the stick is let go)
        walking_ = walk || magnitude < .5;
    if (backward_) {
        // Backpedal: face along the basis, move straight back at the original back speed.
        moveX_ = -s;
        moveY_ = -c;
        heading_ = std::atan2(c, s);
        speed_ = walking_ ? 2.5 : 4.5;
    } else {
        moveX_ = moving ? std::clamp(c * right + s * forward, -1.0, 1.0) : 0;
        moveY_ = moving ? std::clamp(-s * right + c * forward, -1.0, 1.0) : 0;
        const double n = std::hypot(moveX_, moveY_);
        if (n > 1e-6) {
            moveX_ /= n;
            moveY_ /= n;
            heading_ = std::atan2(moveY_, moveX_);
        }
        speed_ = moving ? (walking_ ? 2.5 : runSpeed_) : 0;
    }
    // Enter the water when it is deeper than the float depth; the bottom ends it (step()).
    if (water_ && !swimming_ && *water_ - z_ > swimDepth + .05) {
        swimming_ = true;
        verticalSpeed_ = 0;
    } else if (swimming_ && !water_)
        swimming_ = false;
    if (swimming_) {
        grounded_ = false;
        verticalSpeed_ = 0;
        if (moving)
            speed_ = backward_ ? 2.5 : swimSpeed;
    }
    if (jump && swimming_)
        jump = false; // no jumping out of deep water
    if (jump && grounded_) {
        grounded_ = false;
        verticalSpeed_ = jumpSpeed;
        moved_ = true;
    }
    if (delta) {
        const double seconds = delta * .001;
        const double dx = moveX_ * speed_ * seconds, dy = moveY_ * speed_ * seconds;
        const auto horizontal = uint32_t(std::ceil(std::hypot(dx, dy) / .25));
        const auto steps = std::max<uint32_t>(grounded_ ? std::max(horizontal, 1u) : std::max(horizontal, (uint32_t(delta) + 9) / 10), 1);
        for (uint32_t i = 0; i < steps; ++i)
            step(world, dx / steps, dy / steps, seconds / steps);
    }
    updateEye(world);
}
void WorldPlayer::step(const WorldCollision &world, double dx, double dy, double dt) {
    auto tryMove = [&](double nx, double ny) {
        if (world.wallHit(x_, y_, nx, ny, z_, .45))
            return false;
        const auto ground = world.groundBelow(nx, ny, z_ + (grounded_ ? 1.1 : .3));
        // Outside the streamed terrain only WMO floors (instances: WMO-only maps) carry the player.
        if (!world.hasTerrainAt(nx, ny) && !ground)
            return false;
        if (grounded_) {
            if (!ground)
                return false;
            if (ground->z < z_ - 1.3) {
                grounded_ = false;
                verticalSpeed_ = 0;
            } else
                z_ = ground->z;
        }
        x_ = nx;
        y_ = ny;
        moved_ = true;
        return true;
    };
    if (dx != 0 || dy != 0) {
        if (!tryMove(x_ + dx, y_ + dy) && !(std::abs(dx) > 1e-6 && tryMove(x_ + dx, y_)) &&
            !(std::abs(dy) > 1e-6 && tryMove(x_, y_ + dy)))
            blocked_ = true;
    }
    if (swimming_ && water_) {
        // Float at the surface; the bottom (shore, sea floor) pushes the body up.
        const double surface = *water_ - swimDepth;
        double z = surface;
        if (const auto bottom = world.groundBelow(x_, y_, surface + 1.2); bottom && bottom->z > surface) {
            z = bottom->z;
            swimming_ = false;
            grounded_ = true;
        }
        z_ = z;
        return;
    }
    if (!grounded_) {
        const double before = z_;
        z_ += verticalSpeed_ * dt - .5 * gravity * dt * dt;
        verticalSpeed_ -= gravity * dt;
        const auto ground = world.groundBelow(x_, y_, std::max(before, z_) + .05);
        if (ground && z_ <= ground->z && verticalSpeed_ <= 0) {
            z_ = ground->z;
            verticalSpeed_ = 0;
            grounded_ = true;
        } else if (!ground && z_ < before - 200) {
            // Fell out of the loaded world: return to the last placement height.
            z_ = placeZ_;
            verticalSpeed_ = 0;
        }
    }
}
void WorldPlayer::updateEye(const WorldCollision &world) {
    target_ = {float(x_), float(y_), float(z_ + headHeight)};
    const double fx = std::sin(yaw_), fy = std::cos(yaw_);
    const double horizontal = std::cos(pitch_) * distance_, vertical = std::sin(pitch_) * distance_;
    const C3Vector desired{float(x_ - fx * horizontal), float(y_ - fy * horizontal),
                           float(z_ + headHeight + vertical)};
    const double free = std::clamp(world.rayFree(target_, desired) - .04, .0, 1.0);
    eye_ = {target_.x + float((desired.x - target_.x) * free),
            target_.y + float((desired.y - target_.y) * free),
            target_.z + float((desired.z - target_.z) * free)};
}
C44Matrix WorldPlayer::view() const {
    double f[3]{double(target_.x) - eye_.x, double(target_.y) - eye_.y, double(target_.z) - eye_.z};
    double n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (n < 1e-6) {
        f[0] = std::sin(yaw_);
        f[1] = std::cos(yaw_);
        f[2] = 0;
        n = 1;
    }
    for (double &v : f)
        v /= n;
    double r[3]{f[1], -f[0], 0};
    double rn = std::hypot(r[0], r[1]);
    if (rn < 1e-9) {
        r[0] = 1;
        r[1] = 0;
        rn = 1;
    }
    r[0] /= rn;
    r[1] /= rn;
    const double u[3]{r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    const double e[3]{eye_.x, eye_.y, eye_.z};
    C44Matrix m(1);
    m.a0 = float(r[0]); m.b0 = float(r[1]); m.c0 = float(r[2]);
    m.a1 = float(u[0]); m.b1 = float(u[1]); m.c1 = float(u[2]);
    m.a2 = float(-f[0]); m.b2 = float(-f[1]); m.c2 = float(-f[2]);
    m.d0 = float(-(r[0] * e[0] + r[1] * e[1] + r[2] * e[2]));
    m.d1 = float(-(u[0] * e[0] + u[1] * e[1] + u[2] * e[2]));
    m.d2 = float(f[0] * e[0] + f[1] * e[1] + f[2] * e[2]);
    return m;
}
C44Matrix WorldPlayer::projection(float aspect) const {
    constexpr double f = 1.7320508075688772, nearPlane = .4, farPlane = 1200;
    const double a = std::isfinite(aspect) && aspect > 1e-3f ? aspect : 1;
    C44Matrix m(0);
    m.a0 = float(f / a);
    m.b1 = float(f);
    m.c2 = float(-(farPlane + nearPlane) / (farPlane - nearPlane));
    m.c3 = -1;
    m.d2 = float(-2 * farPlane * nearPlane / (farPlane - nearPlane));
    return m;
}
} // namespace wow::android
