#pragma once
#include "TerrainGeometry.h"
#include <array>
#include <memory>
#include <optional>
namespace wow::android {
struct TerrainSurfaceHit {
    double height = 0, distance = 0;
    C3Vector normal;
    uint32_t chunk = 0, cell = 0, triangle = 0, area = 0;
};
// Immutable terrain queries and development point-ground traversal over drawn triangles.
// Not capsule/world/WMO/liquid collision, navigation or a character controller.
class TerrainSurface {
  public:
    explicit TerrainSurface(std::shared_ptr<const TerrainGeometry> geometry);
    std::optional<TerrainSurfaceHit> heightAt(double worldX, double worldY) const;
    std::optional<TerrainSurfaceHit> raycastDown(double worldX, double worldY, double originZ,
                                                 double maximumDistance) const;
    // Development point-ground traversal, <=1024 horizontal units. Splits at real
    // triangle edges so narrow hole crossings cannot be skipped by endpoint tests.
    // Rejects missing/steep samples and local height changes beyond supplied limits.
    std::optional<TerrainSurfaceHit> traceGround(double x0, double y0, double x1, double y1,
                                                 double maxRise, double maxDrop,
                                                 double minimumNormalZ) const;

  private:
    struct Chunk {
        std::array<double, 9> x, y;
        std::array<uint32_t, 64> cells;
    };
    std::shared_ptr<const TerrainGeometry> geometry_;
    std::array<Chunk, 256> chunks_;
    std::optional<TerrainSurfaceHit> query(double x, double y, double low, double high,
                                           bool ray) const;
};
} // namespace wow::android
