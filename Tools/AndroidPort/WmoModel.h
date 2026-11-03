#pragma once
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "M2PreviewMaterials.h"
#include "WorldCollision.h"
#include <array>
#include <memory>
#include <string>
#include <vector>
namespace wow::android {
class M2PreviewTextures;
class GxStaticMesh;
// WMO v17 map object (root + groups): textured opaque/alpha-key/blended batches with
// vertex positions, normals and UV0. No portals, lights, vertex colors or liquids.
// CPU data is built on the asset worker (archive mounted); draw on the GL thread.
class WmoModel {
  public:
    static WmoModel load(const std::string &rootPath);
    // meshes: optional GL-owned per-group static buffers (created on first draw).
    void draw(const C44Matrix &world, const C44Matrix &view, const C44Matrix &projection,
              M2PreviewTextures *textures,
              std::vector<std::unique_ptr<GxStaticMesh>> *meshes = nullptr) const;
    std::shared_ptr<const M2PreviewMaterials> materials() const noexcept { return materials_; }
    size_t triangles() const noexcept;
    // MOPY collidable faces (F_COLLISION, or rendered non-detail), local space.
    const CollisionShape &collision() const noexcept { return collision_; }
    // Bounding sphere in local space.
    const std::array<float, 3> &center() const noexcept { return center_; }
    float radius() const noexcept { return radius_; }
    // MODS/MODN/MODD placements of doodad set 0 and the requested set (local space).
    struct Doodad {
        std::string path; // .m2
        C44Matrix local{1};
    };
    const std::vector<Doodad> &doodads() const noexcept { return doodads_; }
    static WmoModel load(const std::string &rootPath, uint16_t doodadSet);

  private:
    struct Batch {
        uint32_t firstIndex = 0, indexCount = 0;
        uint16_t minIndex = 0, maxIndex = 0;
        int32_t texture = -1;
        uint32_t blend = 0, flags = 0;
    };
    struct Group {
        std::vector<M2VertexPNT2> vertices;
        std::vector<uint16_t> indices;
        std::vector<Batch> batches;
    };
    mutable std::vector<std::array<float, 4>> groupSpheres_; // model-space culling spheres (lazy)
    std::vector<Group> groups_;
    std::shared_ptr<M2PreviewMaterials> materials_;
    CollisionShape collision_;
    std::array<float, 3> center_{};
    float radius_ = 0;
    std::vector<Doodad> doodads_;
};
} // namespace wow::android
