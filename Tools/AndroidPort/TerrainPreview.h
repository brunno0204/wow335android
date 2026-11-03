#pragma once
#include "Engine/Source/Tempest/C44Matrix.h"
#include "TerrainGeometry.h"
#include "TerrainMaterials.h"
#include "TerrainWalker.h"
#include <memory>
#include <optional>
namespace wow::android {
class TerrainPreviewTextures;
class TerrainSurface;
class GxStaticMesh;
// Static ADT preview with optional basic textures; not world startup, streaming
// or full collision.
class TerrainPreview {
  public:
    explicit TerrainPreview(TerrainGeometry geometry,
                            std::shared_ptr<const TerrainPreviewMaterials> materials = nullptr);
    static TerrainPreview loadInstalled(const std::string &dataRoot, const std::string &map,
                                        uint32_t tileX, uint32_t tileY);
    const TerrainGeometry &geometry() const noexcept { return *geometry_; }
    std::shared_ptr<const TerrainPreviewMaterials> materials() const noexcept { return materials_; }
    std::shared_ptr<const TerrainSurface> surface() const noexcept { return surface_; }
    void enableProbe(bool enabled) noexcept {
        probeEnabled_ = enabled;
        resetFrameClock();
    }
    const TerrainWalker &probe() const noexcept { return *walker_; }
    // World entry: place the probe at a server position, searching up to 12 yards
    // outward for walkable terrain (WMO floors are not modelled). Returns success.
    bool placeProbeNear(double x, double y);
    // Chase camera looks along a WoW facing (radians, 0 = +X, counterclockwise).
    void faceCamera(double facing) noexcept;
    // Horizontal command attempted by the last frame (0,0 when idle); see probe().blocked().
    double moveX() const noexcept { return moveX_; }
    double moveY() const noexcept { return moveY_; }
    // Optional development chase camera. No camera/world collision or smoothing.
    void followProbe(bool enabled) noexcept { followProbe_ = enabled; }
    bool followingProbe() const noexcept {
        return followProbe_ && probeEnabled_ && walker_->placed();
    }
    void resetFrameClock() noexcept { lastFrame_.reset(); }
    void frame(uint64_t milliseconds, double right, double forward, bool jump = false);
    void orbit(float horizontalPixels, float verticalPixels);
    C44Matrix projection(float aspect) const;
    C44Matrix view() const;
    void draw(float aspect,
              TerrainPreviewTextures *textures = nullptr, bool showProbe = true) const;
    // External camera; chunks farther than range from focus (x, y) are skipped.
    void drawWith(const C44Matrix &view, const C44Matrix &projection, TerrainPreviewTextures *textures,
                  bool showProbe, bool perspective, const double *focus, double range,
                  GxStaticMesh *mesh = nullptr, double detailRange = 0) const;
  private:
    std::shared_ptr<const TerrainGeometry> geometry_;
    std::shared_ptr<const TerrainPreviewMaterials> materials_;
    std::shared_ptr<const TerrainSurface> surface_;
    std::optional<TerrainWalker> walker_;
    std::optional<uint64_t> lastFrame_;
    bool probeEnabled_ = false, followProbe_ = false;
    mutable std::vector<float> chunkRadius_; // frustum culling spheres (lazy)
    double followPitch_ = .6, moveX_ = 0, moveY_ = 0;
    double center_[3]{}, radius_ = 1, yaw_ = .4, pitch_ = .8;
};
} // namespace wow::android
