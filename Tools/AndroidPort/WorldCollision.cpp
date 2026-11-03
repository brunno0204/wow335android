#include "WorldCollision.h"
#include "M2File.h"
#include "TerrainSurface.h"
#include "WorldAssets.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
namespace wow::android {
CollisionShape m2CollisionShape(const std::vector<uint8_t> &b) {
    CollisionShape shape;
    const size_t header = m2::modelHeader(b);
    const uint32_t indexCount = m2::u32(b, 0xd8), indexOffset = m2::u32(b, 0xdc);
    const uint32_t vertexCount = m2::u32(b, 0xe0), vertexOffset = m2::u32(b, 0xe4);
    if (!indexCount || !vertexCount || indexCount > 300000 || vertexCount > 100000)
        return shape;
    if (indexOffset < header || vertexOffset < header || indexOffset > b.size() ||
        vertexOffset > b.size() || indexCount > (b.size() - indexOffset) / 2 ||
        vertexCount > (b.size() - vertexOffset) / 12)
        return shape;
    shape.vertices.resize(vertexCount);
    std::memcpy(shape.vertices.data(), b.data() + vertexOffset, size_t(vertexCount) * 12);
    for (const auto &v : shape.vertices)
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
            return {};
    for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
        const uint16_t a = m2::u16(b, indexOffset + i * 2), c = m2::u16(b, indexOffset + i * 2 + 2),
                       d = m2::u16(b, indexOffset + i * 2 + 4);
        if (a < vertexCount && c < vertexCount && d < vertexCount)
            shape.indices.insert(shape.indices.end(), {a, c, d});
    }
    return shape;
}
void CollisionMesh::append(const CollisionShape &shape, const C44Matrix &m) {
    auto xf = [&](const C3Vector &v) {
        return C3Vector{v.x * m.a0 + v.y * m.b0 + v.z * m.c0 + m.d0,
                        v.x * m.a1 + v.y * m.b1 + v.z * m.c1 + m.d1,
                        v.x * m.a2 + v.y * m.b2 + v.z * m.c2 + m.d2};
    };
    for (size_t i = 0; i + 2 < shape.indices.size(); i += 3) {
        Triangle t;
        t.a = xf(shape.vertices[shape.indices[i]]);
        t.b = xf(shape.vertices[shape.indices[i + 1]]);
        t.c = xf(shape.vertices[shape.indices[i + 2]]);
        const double ux = t.b.x - t.a.x, uy = t.b.y - t.a.y, uz = t.b.z - t.a.z;
        const double vx = t.c.x - t.a.x, vy = t.c.y - t.a.y, vz = t.c.z - t.a.z;
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (!(length > 1e-9) || !std::isfinite(length))
            continue;
        t.n = {float(nx / length), float(ny / length), float(nz / length)};
        t.minZ = std::min({t.a.z, t.b.z, t.c.z});
        t.maxZ = std::max({t.a.z, t.b.z, t.c.z});
        triangles_.push_back(t);
    }
}
void CollisionMesh::build() {
    if (triangles_.empty())
        return;
    minX = minY = minZ = std::numeric_limits<float>::max();
    maxX = maxY = maxZ = -std::numeric_limits<float>::max();
    for (const auto &t : triangles_)
        for (const auto *p : {&t.a, &t.b, &t.c}) {
            minX = std::min(minX, p->x);
            minY = std::min(minY, p->y);
            minZ = std::min(minZ, p->z);
            maxX = std::max(maxX, p->x);
            maxY = std::max(maxY, p->y);
            maxZ = std::max(maxZ, p->z);
        }
    columns_ = std::max(1u, uint32_t(std::ceil((maxX - minX) / cell_)) + 1);
    rows_ = std::max(1u, uint32_t(std::ceil((maxY - minY) / cell_)) + 1);
    if (uint64_t(columns_) * rows_ > 4'000'000) { // pathological spread: one cell
        columns_ = rows_ = 1;
    }
    grid_.assign(size_t(columns_) * rows_, {});
    for (uint32_t i = 0; i < triangles_.size(); ++i) {
        const auto &t = triangles_[i];
        const float x0 = std::min({t.a.x, t.b.x, t.c.x}), x1 = std::max({t.a.x, t.b.x, t.c.x});
        const float y0 = std::min({t.a.y, t.b.y, t.c.y}), y1 = std::max({t.a.y, t.b.y, t.c.y});
        const uint32_t c0 = std::min(columns_ - 1, uint32_t((x0 - minX) / cell_)),
                       c1 = std::min(columns_ - 1, uint32_t((x1 - minX) / cell_)),
                       r0 = std::min(rows_ - 1, uint32_t((y0 - minY) / cell_)),
                       r1 = std::min(rows_ - 1, uint32_t((y1 - minY) / cell_));
        for (uint32_t r = r0; r <= r1; ++r)
            for (uint32_t c = c0; c <= c1; ++c)
                grid_[size_t(r) * columns_ + c].push_back(i);
    }
    stamp_.assign(triangles_.size(), 0);
}
void WorldCollision::clear() {
    terrain_.clear();
    meshes_.clear();
}
void WorldCollision::addTerrain(uint32_t key, std::shared_ptr<const TerrainSurface> surface) {
    terrain_[key] = std::move(surface);
}
void WorldCollision::removeTerrain(uint32_t key) { terrain_.erase(key); }
void WorldCollision::addMesh(uint64_t id, std::shared_ptr<const CollisionMesh> mesh) {
    if (mesh && !mesh->empty())
        meshes_[id] = std::move(mesh);
}
void WorldCollision::removeMesh(uint64_t id) { meshes_.erase(id); }
const TerrainSurface *WorldCollision::terrainAt(double x, double y) const {
    try {
        const auto tile = terrainTileFor(x, y);
        const auto found = terrain_.find(tile.first * 64 + tile.second);
        return found == terrain_.end() ? nullptr : found->second.get();
    } catch (const std::exception &) {
        return nullptr;
    }
}
bool WorldCollision::hasTerrainAt(double x, double y) const { return terrainAt(x, y) != nullptr; }
uint32_t WorldCollision::areaAt(double x, double y) const {
    if (const auto *surface = terrainAt(x, y))
        if (const auto hit = surface->heightAt(x, y))
            return hit->area;
    return 0;
}
namespace {
// Height of the triangle's plane at (x, y) if the point lies inside its XY projection.
std::optional<double> heightOn(const CollisionMesh::Triangle &t, double x, double y) {
    const double v0x = t.c.x - t.a.x, v0y = t.c.y - t.a.y, v1x = t.b.x - t.a.x,
                 v1y = t.b.y - t.a.y, v2x = x - t.a.x, v2y = y - t.a.y;
    const double det = v0x * v1y - v1x * v0y;
    if (std::abs(det) < 1e-12)
        return std::nullopt;
    const double u = (v2x * v1y - v1x * v2y) / det, v = (v0x * v2y - v2x * v0y) / det;
    constexpr double e = -1e-6;
    if (u < e || v < e || u + v > 1 - e)
        return std::nullopt;
    return t.a.z + u * (t.c.z - t.a.z) + v * (t.b.z - t.a.z);
}
// Moller-Trumbore; returns the parametric distance along dir (0..1 for a segment).
std::optional<double> segment(const CollisionMesh::Triangle &t, const double o[3], const double d[3]) {
    const double e1[3]{t.b.x - t.a.x, t.b.y - t.a.y, t.b.z - t.a.z};
    const double e2[3]{t.c.x - t.a.x, t.c.y - t.a.y, t.c.z - t.a.z};
    const double p[3]{d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                      d[0] * e2[1] - d[1] * e2[0]};
    const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::abs(det) < 1e-12)
        return std::nullopt;
    const double inv = 1 / det;
    const double s[3]{o[0] - t.a.x, o[1] - t.a.y, o[2] - t.a.z};
    const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0 || u > 1)
        return std::nullopt;
    const double q[3]{s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2],
                      s[0] * e1[1] - s[1] * e1[0]};
    const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < 0 || u + v > 1)
        return std::nullopt;
    const double k = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    if (k < 0 || k > 1)
        return std::nullopt;
    return k;
}
} // namespace
std::optional<GroundHit> WorldCollision::groundBelow(double x, double y, double top,
                                                     double minNormal) const {
    std::optional<GroundHit> best;
    // Float seams between chunks/tiles can leave hairline gaps: retry nudged points.
    constexpr double nudge[5][2]{{0, 0}, {.01, 0}, {-.01, 0}, {0, .01}, {0, -.01}};
    for (const auto &n : nudge) {
        const double qx = x + n[0], qy = y + n[1];
        const auto *surface = terrainAt(qx, qy);
        if (!surface)
            continue;
        const auto hit = surface->raycastDown(qx, qy, top, 4000);
        if (!hit)
            continue;
        if (hit->normal.z >= minNormal)
            best = GroundHit{hit->height, hit->normal.z};
        break;
    }
    for (const auto &[id, mesh] : meshes_) {
        (void)id;
        if (x < mesh->minX || x > mesh->maxX || y < mesh->minY || y > mesh->maxY || mesh->minZ > top)
            continue;
        mesh->visit(x, y, x, y, [&](const CollisionMesh::Triangle &t) {
            if (std::abs(t.n.z) < minNormal || t.minZ > top)
                return;
            const auto z = heightOn(t, x, y);
            if (z && *z <= top + 1e-4 && (!best || *z > best->z))
                best = GroundHit{*z, std::abs(t.n.z)};
        });
    }
    return best;
}
bool WorldCollision::wallHit(double x0, double y0, double x1, double y1, double z,
                             double radius) const {
    double dx = x1 - x0, dy = y1 - y0;
    const double length = std::hypot(dx, dy);
    if (length < 1e-9)
        return false;
    dx = dx / length * (length + radius);
    dy = dy / length * (length + radius);
    const double lo[2]{std::min(x0, x0 + dx), std::min(y0, y0 + dy)},
        hi[2]{std::max(x0, x0 + dx), std::max(y0, y0 + dy)};
    bool hit = false;
    for (const auto &[id, mesh] : meshes_) {
        (void)id;
        if (hi[0] < mesh->minX || lo[0] > mesh->maxX || hi[1] < mesh->minY || lo[1] > mesh->maxY ||
            z + 2.2 < mesh->minZ || z > mesh->maxZ)
            continue;
        mesh->visit(lo[0], lo[1], hi[0], hi[1], [&](const CollisionMesh::Triangle &t) {
            if (hit || std::abs(t.n.z) >= .5 || t.maxZ < z + 1.0 || t.minZ > z + 2.2)
                return;
            for (double h : {1.1, 1.9}) {
                const double o[3]{x0, y0, z + h}, d[3]{dx, dy, 0};
                if (segment(t, o, d)) {
                    hit = true;
                    return;
                }
            }
        });
        if (hit)
            return true;
    }
    return false;
}
double WorldCollision::rayFree(const C3Vector &from, const C3Vector &to) const {
    const double o[3]{from.x, from.y, from.z}, d[3]{to.x - from.x, to.y - from.y, to.z - from.z};
    double best = 1;
    const double lo[2]{std::min(from.x, to.x), std::min(from.y, to.y)},
        hi[2]{std::max(from.x, to.x), std::max(from.y, to.y)};
    for (const auto &[id, mesh] : meshes_) {
        (void)id;
        if (hi[0] < mesh->minX || lo[0] > mesh->maxX || hi[1] < mesh->minY || lo[1] > mesh->maxY)
            continue;
        mesh->visit(lo[0], lo[1], hi[0], hi[1], [&](const CollisionMesh::Triangle &t) {
            if (auto k = segment(t, o, d); k && *k < best)
                best = *k;
        });
    }
    // Terrain: march the ray and stop where it dips below the ground.
    const int steps = 24;
    for (int i = 1; i <= steps; ++i) {
        const double k = best * i / steps;
        const double x = o[0] + d[0] * k, y = o[1] + d[1] * k, z = o[2] + d[2] * k;
        if (const auto *surface = terrainAt(x, y))
            if (auto h = surface->heightAt(x, y); h && z < h->height + .25 && i > 1) {
                best = std::max(0.0, best * (i - 1) / steps);
                break;
            }
    }
    return best;
}
} // namespace wow::android
