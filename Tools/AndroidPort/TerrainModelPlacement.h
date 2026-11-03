#pragma once
#include "Engine/Source/Tempest/C44Matrix.h"
#include "TerrainWalker.h"
#include <optional>
namespace wow::android {
// Upright development M2 placement: model origin at probe feet, local +X forward.
// No character bounds, model fitting, terrain tilt or animation state selection.
class TerrainModelPlacement {
  public:
    void update(const TerrainWalker &probe);
    bool ready() const noexcept { return previous_.has_value(); }
    const C44Matrix &world() const noexcept { return world_; }
    double heading() const noexcept { return heading_; }

  private:
    std::optional<TerrainProbePosition> previous_;
    double heading_ = 0;
    C44Matrix world_{1};
};
} // namespace wow::android
