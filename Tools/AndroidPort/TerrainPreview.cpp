#include "TerrainPreview.h"
#include "ClientData.h"
#include "M2File.h"
#include "TerrainAlpha.h"
#include "TerrainSurface.h"
#include <algorithm>
#include <cmath>
namespace wow::android {
TerrainPreview::TerrainPreview(TerrainGeometry geometry,
                               std::shared_ptr<const TerrainPreviewMaterials> materials)
    : geometry_(std::make_shared<const TerrainGeometry>(std::move(geometry))),
      materials_(std::move(materials)) {
    m2::require(geometry_->vertices.size() == 256 * 145 && geometry_->chunks.size() == 256,
                "Invalid terrain preview geometry");
    m2::require(geometry_->indices.size() <= 256 * 768 && geometry_->indices.size() % 3 == 0,
                "Invalid terrain preview triangle count");
    if (materials_) {
        m2::require(materials_->chunks.size() == geometry_->chunks.size(),
                    "Mismatched terrain materials");
        m2::require(materials_->textures.size() <= 1280, "Terrain texture count exceeds budget");
        m2::require(materials_->clamp.empty() ||
                        materials_->clamp.size() == materials_->textures.size(),
                    "Invalid terrain sampler metadata");
        uint64_t bytes = 0;
        for (const auto &image : materials_->textures) {
            m2::require(bool(image), "Missing terrain texture pixels");
            const uint64_t pixels = uint64_t(image->width) * image->height;
            m2::require(image->width && !(image->width & (image->width - 1)) && image->height &&
                            !(image->height & (image->height - 1)) &&
                            pixels <= (128 * 1024 * 1024 - bytes) / 4 &&
                            pixels * 4 == image->rgba.size(),
                        "Invalid terrain texture pixels/budget");
            bytes += image->rgba.size();
        }
        for (const auto &chunk : materials_->chunks) {
            m2::require(chunk.texture < 0 || size_t(chunk.texture) < materials_->textures.size(),
                        "Invalid terrain material texture");
            m2::require(chunk.passes.size() <= 4, "Invalid terrain pass count");
            if (chunk.texture >= 0)
                m2::require(!chunk.passes.empty() && chunk.passes[0].texture == chunk.texture,
                            "Missing terrain base pass");
            else
                m2::require(chunk.passes.empty(), "Fallback has terrain passes");
            for (const auto &pass : chunk.passes)
                m2::require(pass.texture >= 0 &&
                                size_t(pass.texture) < materials_->textures.size() &&
                                (pass.mask < 0 || size_t(pass.mask) < materials_->textures.size()),
                            "Invalid terrain material pass");
        }
    }
    uint32_t indexEnd = 0;
    for (size_t i = 0; i < geometry_->chunks.size(); ++i) {
        const auto &chunk = geometry_->chunks[i];
        m2::require(chunk.x == i % 16 && chunk.y == i / 16 && chunk.firstVertex == i * 145 &&
                        chunk.firstIndex == indexEnd && chunk.indexCount <= 768 &&
                        chunk.indexCount % 12 == 0 &&
                        chunk.indexCount <= geometry_->indices.size() - indexEnd,
                    "Invalid terrain draw ranges");
        for (size_t j = indexEnd; j < indexEnd + chunk.indexCount; ++j)
            m2::require(geometry_->indices[j] >= chunk.firstVertex &&
                            geometry_->indices[j] < chunk.firstVertex + 145,
                        "Terrain index exceeds chunk vertices");
        indexEnd += chunk.indexCount;
    }
    m2::require(indexEnd == geometry_->indices.size(), "Unreferenced terrain indices");
    double low[3]{INFINITY, INFINITY, INFINITY}, high[3]{-INFINITY, -INFINITY, -INFINITY};
    for (const auto &vertex : geometry_->vertices) {
        m2::require(std::isfinite(vertex.uv.x) && std::isfinite(vertex.uv.y),
                    "Nonfinite terrain UV");
        m2::require(std::isfinite(vertex.maskUv.x) && std::isfinite(vertex.maskUv.y),
                    "Nonfinite terrain alpha UV");
        m2::require(std::isfinite(vertex.normal.x) && std::isfinite(vertex.normal.y) &&
                        std::isfinite(vertex.normal.z),
                    "Nonfinite terrain preview normal");
        const double values[]{vertex.position.x, vertex.position.y, vertex.position.z};
        for (unsigned k = 0; k < 3; ++k) {
            m2::require(std::isfinite(values[k]), "Nonfinite terrain preview bounds");
            low[k] = std::min(low[k], values[k]);
            high[k] = std::max(high[k], values[k]);
        }
    }
    for (uint16_t index : geometry_->indices)
        m2::require(index < geometry_->vertices.size(), "Invalid terrain preview index");
    double squared = 0;
    for (unsigned k = 0; k < 3; ++k) {
        center_[k] = (low[k] + high[k]) * .5;
        const double extent = (high[k] - low[k]) * .5;
        squared += extent * extent;
    }
    radius_ = std::sqrt(squared);
    m2::require(std::isfinite(radius_) && radius_ >= 1e-6, "Degenerate terrain preview");
    surface_ = std::make_shared<TerrainSurface>(geometry_);
    walker_.emplace(surface_);
    if (!walker_->place(center_[0], center_[1]))
        for (const auto &chunk : geometry_->chunks) {
            if (!chunk.indexCount)
                continue;
            const auto &a = geometry_->vertices[geometry_->indices[chunk.firstIndex]].position;
            const auto &b = geometry_->vertices[geometry_->indices[chunk.firstIndex + 1]].position;
            const auto &c = geometry_->vertices[geometry_->indices[chunk.firstIndex + 2]].position;
            if (walker_->place((double(a.x) + b.x + c.x) / 3, (double(a.y) + b.y + c.y) / 3))
                break;
        }
}
void TerrainPreview::frame(uint64_t milliseconds, double right, double forward, bool jump) {
    m2::require(std::isfinite(right) && std::isfinite(forward) && std::abs(right) <= 1 &&
                    std::abs(forward) <= 1,
                "Invalid terrain preview movement");
    const auto delta = lastFrame_ && milliseconds >= *lastFrame_
                           ? std::min<uint64_t>(milliseconds - *lastFrame_, 100)
                           : 0;
    if (probeEnabled_) {
        if (jump)
            walker_->jump();
        const double magnitude = std::hypot(right, forward);
        if (magnitude > 1) {
            right /= magnitude;
            forward /= magnitude;
        }
        const double c = std::cos(yaw_), s = std::sin(yaw_);
        moveX_ = std::clamp(c * right + s * forward, -1.0, 1.0);
        moveY_ = std::clamp(-s * right + c * forward, -1.0, 1.0);
        walker_->advance(uint32_t(delta), moveX_, moveY_);
    } else
        moveX_ = moveY_ = 0;
    lastFrame_ = milliseconds;
}
bool TerrainPreview::placeProbeNear(double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y))
        return false;
    if (walker_->place(x, y))
        return true;
    for (int radius = 1; radius <= 12; ++radius)
        for (int step = 0; step < 16; ++step) {
            const double angle = step * 3.14159265358979323846 / 8;
            if (walker_->place(x + radius * std::cos(angle), y + radius * std::sin(angle)))
                return true;
        }
    return false;
}
void TerrainPreview::faceCamera(double facing) noexcept {
    // frame(): forward command moves along (sin(yaw), cos(yaw)) => facing = pi/2 - yaw.
    if (std::isfinite(facing))
        yaw_ = std::remainder(3.14159265358979323846 / 2 - facing, 2 * 3.14159265358979323846);
}
TerrainPreview TerrainPreview::loadInstalled(const std::string &dataRoot, const std::string &map,
                                             uint32_t tileX, uint32_t tileY) {
    const auto path = terrainTilePath(map, tileX, tileY);
    const auto mount = sharedClientData(dataRoot);
        const auto &install = *mount;
    m2::require(install.report().ready, "Local game data is incomplete");
    auto geometry = loadTerrainGeometry(path, tileX, tileY, true);
    if (terrainNeedsAlphaMode(geometry))
        geometry.alphaMode = loadTerrainAlphaMode(map, tileX, tileY);
    auto materials = std::make_shared<TerrainPreviewMaterials>(prepareTerrainMaterials(geometry));
    return TerrainPreview(std::move(geometry), std::move(materials));
}
void TerrainPreview::orbit(float x, float y) {
    m2::require(std::isfinite(x) && std::isfinite(y), "Invalid terrain orbit");
    constexpr double pi = 3.14159265358979323846;
    yaw_ = std::remainder(yaw_ + std::remainder(double(x) * .005, 2 * pi), 2 * pi);
    if (followingProbe())
        followPitch_ = std::clamp(followPitch_ + double(y) * .005, .1, 1.35);
    else
        pitch_ = std::clamp(pitch_ + double(y) * .005, -1.4, 1.4);
}
C44Matrix TerrainPreview::view() const {
    if (!followingProbe())
        return C44Matrix(1);
    const auto &p = walker_->position();
    const double c = std::cos(yaw_), s = std::sin(yaw_), cp = std::cos(followPitch_),
                 sp = std::sin(followPitch_);
    // Right/up/back basis, Z-up, looking at a point two units above the probe.
    // Eye is 36 units behind the target; translation uses double intermediates.
    C44Matrix m(1);
    m.a0 = float(c);
    m.b0 = float(-s);
    m.c0 = 0;
    m.a1 = float(s * sp);
    m.b1 = float(c * sp);
    m.c1 = float(cp);
    m.a2 = float(-s * cp);
    m.b2 = float(-c * cp);
    m.c2 = float(sp);
    m.d0 = float(-p.x * c + p.y * s);
    m.d1 = float(-p.x * s * sp - p.y * c * sp - (p.z + 2) * cp);
    m.d2 = float(p.x * s * cp + p.y * c * cp - (p.z + 2) * sp - 36);
    return m;
}
C44Matrix TerrainPreview::projection(float aspect) const {
    m2::require(std::isfinite(aspect) && aspect >= 1e-3f && aspect <= 1e3f,
                "Invalid terrain preview aspect");
    if (followingProbe()) {
        // 60-degree vertical field of view, OpenGL clip depth [-1,1]. The Gx
        // adapter's paired view/projection Z conversions cancel for this basis.
        constexpr double f = 1.7320508075688772, nearPlane = .25, farPlane = 1024;
        C44Matrix m(0);
        m.a0 = float(f / aspect);
        m.b1 = float(f);
        m.c2 = float(-(farPlane + nearPlane) / (farPlane - nearPlane));
        m.c3 = -1;
        m.d2 = float(-2 * farPlane * nearPlane / (farPlane - nearPlane));
        m.d3 = 0;
        return m;
    }
    const double sx = .85 / radius_ / std::max(1.0, double(aspect));
    const double sy = .85 / radius_ * std::min(1.0, double(aspect)), sz = .5 / radius_;
    const double c = std::cos(yaw_), s = std::sin(yaw_), cp = std::cos(pitch_),
                 sp = std::sin(pitch_);
    C44Matrix m(1);
    m.a0 = float(c * sx);
    m.b0 = float(-s * sx);
    m.c0 = 0;
    m.a1 = float(s * sp * sy);
    m.b1 = float(c * sp * sy);
    m.c1 = float(cp * sy);
    m.a2 = float(s * cp * sz);
    m.b2 = float(c * cp * sz);
    m.c2 = float(-sp * sz);
    m.d0 = float(-center_[0] * m.a0 - center_[1] * m.b0 - center_[2] * m.c0);
    m.d1 = float(-center_[0] * m.a1 - center_[1] * m.b1 - center_[2] * m.c1);
    m.d2 = float(-center_[0] * m.a2 - center_[1] * m.b2 - center_[2] * m.c2);
    return m;
}
} // namespace wow::android
