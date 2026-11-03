#include "TerrainWalker.h"
#include "M2File.h"
#include <algorithm>
#include <cmath>
namespace wow::android {
TerrainWalker::TerrainWalker(std::shared_ptr<const TerrainSurface> surface)
    : surface_(std::move(surface)) {
    m2::require(bool(surface_), "Missing terrain walker surface");
}
bool TerrainWalker::place(double x, double y) {
    const auto hit = surface_->heightAt(x, y);
    if (!hit || hit->normal.z < .65)
        return false;
    position_ = {x, y, hit->height};
    groundHeight_ = hit->height;
    verticalSpeed_ = 0;
    grounded_ = true;
    placed_ = true;
    blocked_ = false;
    return true;
}
bool TerrainWalker::jump() noexcept {
    if (!onGround())
        return false;
    grounded_ = false;
    verticalSpeed_ = 10;
    return true;
}
void TerrainWalker::advance(uint32_t milliseconds, double x, double y) {
    m2::require(milliseconds <= 100 && std::isfinite(x) && std::isfinite(y) && std::abs(x) <= 1 &&
                    std::abs(y) <= 1,
                "Invalid terrain probe movement");
    blocked_ = false;
    if (!placed_ || !milliseconds || (grounded_ && x == 0 && y == 0))
        return;
    const double magnitude = std::hypot(x, y);
    if (magnitude > 1) {
        x /= magnitude;
        y /= magnitude;
    }
    const double dx = x * 7 * milliseconds * .001, dy = y * 7 * milliseconds * .001;
    const auto horizontalSteps = uint32_t(std::ceil(std::hypot(dx, dy) / .25));
    const auto steps =
        grounded_ ? horizontalSteps : std::max(horizontalSteps, (milliseconds + 9) / 10);
    if (!steps)
        return;
    const double dt = milliseconds * .001 / steps;
    for (uint32_t i = 0; i < steps; ++i) {
        if (!blocked_ && (dx != 0 || dy != 0)) {
            const double nextX = position_.x + dx / steps, nextY = position_.y + dy / steps;
            const auto hit =
                surface_->traceGround(position_.x, position_.y, nextX, nextY, .6, 1, .65);
            if (!hit) {
                blocked_ = true;
            } else {
                position_.x = nextX;
                position_.y = nextY;
                groundHeight_ = hit->height;
            }
        }
        if (grounded_) {
            position_.z = groundHeight_;
        } else {
            position_.z += verticalSpeed_ * dt - 10 * dt * dt;
            verticalSpeed_ -= 20 * dt;
            if (position_.z <= groundHeight_ + 1e-7) {
                position_.z = groundHeight_;
                verticalSpeed_ = 0;
                grounded_ = true;
            }
        }
    }
}
void TerrainProbeInput::key(KEY key, bool down) noexcept {
    switch (key) {
    case KEY_W:
        forward_ = down;
        break;
    case KEY_S:
        back_ = down;
        break;
    case KEY_Q:
        left_ = down;
        break;
    case KEY_E:
        right_ = down;
        break;
    case KEY_SPACE:
        if (down && !jumpHeld_)
            jumpPressed_ = true;
        jumpHeld_ = down;
        break;
    default:
        break;
    }
}
} // namespace wow::android
