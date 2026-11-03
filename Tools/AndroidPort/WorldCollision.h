#pragma once
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>
namespace wow::android {
class TerrainSurface;
// Local collision triangles of one model (WMO MOPY-collidable faces or M2 bounding mesh).
struct CollisionShape {
    std::vector<C3Vector> vertices;
    std::vector<uint32_t> indices; // triangle list
};
// M2 header collision mesh (m_collisionIndices / m_collisionPositions).
CollisionShape m2CollisionShape(const std::vector<uint8_t> &model);
// World-space triangle soup with a 2D uniform grid; built on the asset worker.
class CollisionMesh {
  public:
    void append(const CollisionShape &shape, const C44Matrix &world);
    void build(); // after the last append
    bool empty() const noexcept { return triangles_.empty(); }
    size_t triangles() const noexcept { return triangles_.size(); }
    struct Triangle {
        C3Vector a, b, c, n; // n: unit normal
        float minZ, maxZ;
    };
    template <class F> void visit(double minX, double minY, double maxX, double maxY, F &&f) const;
    float minX = 0, minY = 0, maxX = 0, maxY = 0, minZ = 0, maxZ = 0;

  private:
    std::vector<Triangle> triangles_;
    static constexpr float cell_ = 8;
    uint32_t columns_ = 0, rows_ = 0;
    std::vector<std::vector<uint32_t>> grid_;
    mutable std::vector<uint32_t> stamp_;
    mutable uint32_t visit_ = 0;
};
struct GroundHit {
    double z = 0, normalZ = 1;
};
// GL-thread world collision: streamed ADT terrain surfaces and object meshes. A simple
// development character controller model (ground below, walls, camera ray), not the
// original CWorldCollision/CMovement_C.
class WorldCollision {
  public:
    void clear();
    void addTerrain(uint32_t key, std::shared_ptr<const TerrainSurface> surface);
    void removeTerrain(uint32_t key);
    void addMesh(uint64_t id, std::shared_ptr<const CollisionMesh> mesh);
    void removeMesh(uint64_t id);
    bool hasTerrainAt(double x, double y) const;
    // AreaTable ID of the terrain chunk under (x, y) (MCNK areaId), 0 when unknown.
    uint32_t areaAt(double x, double y) const;
    // Highest walkable surface (|normal.z| >= minNormal) at or below top.
    std::optional<GroundHit> groundBelow(double x, double y, double top, double minNormal = .5) const;
    // Any steep face crossed by the horizontal segment at heights z+low..z+high.
    bool wallHit(double x0, double y0, double x1, double y1, double z, double radius) const;
    // Fraction of the eye ray (target -> eye) that is free of geometry, 0..1.
    double rayFree(const C3Vector &from, const C3Vector &to) const;

  private:
    std::map<uint32_t, std::shared_ptr<const TerrainSurface>> terrain_;
    std::map<uint64_t, std::shared_ptr<const CollisionMesh>> meshes_;
    const TerrainSurface *terrainAt(double x, double y) const;
};
template <class F>
void CollisionMesh::visit(double x0, double y0, double x1, double y1, F &&f) const {
    if (triangles_.empty() || x1 < minX || y1 < minY || x0 > maxX || y0 > maxY)
        return;
    if (stamp_.size() != triangles_.size())
        stamp_.assign(triangles_.size(), 0);
    if (++visit_ == 0) {
        std::fill(stamp_.begin(), stamp_.end(), 0);
        visit_ = 1;
    }
    auto col = [&](double x) {
        return uint32_t(std::min<double>(columns_ - 1, std::max(0.0, (x - minX) / cell_)));
    };
    auto row = [&](double y) {
        return uint32_t(std::min<double>(rows_ - 1, std::max(0.0, (y - minY) / cell_)));
    };
    for (uint32_t r = row(y0); r <= row(y1); ++r)
        for (uint32_t c = col(x0); c <= col(x1); ++c)
            for (uint32_t t : grid_[size_t(r) * columns_ + c]) {
                if (stamp_[t] == visit_)
                    continue;
                stamp_[t] = visit_;
                f(triangles_[t]);
            }
}
} // namespace wow::android
