#include "TerrainModelPlacement.h"
#include "M2File.h"
#include <cmath>
namespace wow::android {
void TerrainModelPlacement::update(const TerrainWalker &probe) {
    if (!probe.placed()) {
        previous_.reset();
        heading_ = 0;
        world_ = C44Matrix(1);
        return;
    }
    const auto &p = probe.position();
    m2::require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z),
                "Invalid terrain model position");
    if (previous_) {
        const double dx = p.x - previous_->x, dy = p.y - previous_->y;
        if (std::hypot(dx, dy) > 1e-9)
            heading_ = std::atan2(dy, dx);
    }
    const float c = float(std::cos(heading_)), s = float(std::sin(heading_));
    world_ = C44Matrix(1);
    world_.a0 = c;
    world_.a1 = s;
    world_.b0 = -s;
    world_.b1 = c;
    world_.d0 = float(p.x);
    world_.d1 = float(p.y);
    world_.d2 = float(p.z);
    previous_ = p;
}
} // namespace wow::android
