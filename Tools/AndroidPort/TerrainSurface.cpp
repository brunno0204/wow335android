#include "TerrainSurface.h"
#include "M2File.h"
#include "WoW/Source/WorldClient/WorldConstants.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace wow::android {
TerrainSurface::TerrainSurface(std::shared_ptr<const TerrainGeometry> geometry)
    : geometry_(std::move(geometry)) {
    m2::require(geometry_ && geometry_->chunks.size() == 256 &&
                    geometry_->vertices.size() == 256 * 145 &&
                    geometry_->indices.size() <= 256 * 768,
                "Invalid terrain surface geometry");
    uint32_t end = 0;
    constexpr uint32_t fan[]{9, 0, 17, 9, 1, 0, 9, 18, 1, 9, 17, 18};
    for (uint32_t i = 0; i < 256; ++i) {
        const auto &source = geometry_->chunks[i];
        auto &out = chunks_[i];
        m2::require(source.x == i % 16 && source.y == i / 16 && source.firstVertex == i * 145 &&
                        source.firstIndex == end && source.indexCount <= 768 &&
                        source.indexCount <= geometry_->indices.size() - end,
                    "Invalid terrain surface ranges");
        const auto &origin = geometry_->vertices[i * 145].position;
        uint32_t sample = 0;
        // Require the checked grid used by the decoder; preserve actual float XY
        // values for queries, including rounding at chunk/tile seams.
        for (uint32_t row = 0; row < 9; ++row) {
            auto validate = [&](uint32_t col, bool center) {
                const auto &p = geometry_->vertices[i * 145 + sample++].position;
                const float half = center ? HALF_SMOLTILE_SIZE : 0;
                m2::require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
                                p.x == origin.x - float(row) * SMOLTILE_SIZE - half &&
                                p.y == origin.y - float(col) * SMOLTILE_SIZE - half,
                            "Invalid terrain surface grid");
            };
            for (uint32_t col = 0; col < 9; ++col)
                validate(col, false);
            if (row < 8)
                for (uint32_t col = 0; col < 8; ++col)
                    validate(col, true);
            out.x[row] = geometry_->vertices[i * 145 + row * 17].position.x;
            out.y[row] = geometry_->vertices[i * 145 + row].position.y;
            if (row)
                m2::require(out.x[row] < out.x[row - 1] && out.y[row] < out.y[row - 1],
                            "Degenerate terrain surface cell");
        }
        for (uint32_t row = 0; row < 8; ++row)
            for (uint32_t col = 0; col < 8; ++col) {
                auto &cell = out.cells[row * 8 + col];
                cell = UINT32_MAX;
                if (source.holes & (1u << ((row / 2) * 4 + col / 2)))
                    continue;
                m2::require(end <= geometry_->indices.size() &&
                                geometry_->indices.size() - end >= 12,
                            "Truncated terrain surface triangles");
                cell = end;
                for (uint32_t j : fan)
                    m2::require(geometry_->indices[end++] == i * 145 + row * 17 + col + j,
                                "Terrain surface differs from rendered topology");
            }
        m2::require(end == source.firstIndex + source.indexCount,
                    "Terrain surface hole/index mismatch");
    }
    m2::require(end == geometry_->indices.size(), "Unreferenced terrain surface indices");
}
std::optional<TerrainSurfaceHit> TerrainSurface::traceGround(double x0, double y0, double x1,
                                                             double y1, double maxRise,
                                                             double maxDrop,
                                                             double minNormal) const {
    m2::require(std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1) &&
                    std::isfinite(maxRise) && std::isfinite(maxDrop) && std::isfinite(minNormal) &&
                    maxRise >= 0 && maxDrop >= 0 && minNormal >= 0 && minNormal <= 1,
                "Invalid terrain ground trace");
    const double dx = x1 - x0, dy = y1 - y0;
    m2::require(std::hypot(dx, dy) <= 1024, "Terrain ground trace exceeds distance budget");
    auto previous = heightAt(x0, y0);
    if (!previous || previous->normal.z < minNormal)
        return std::nullopt;
    if (dx == 0 && dy == 0)
        return previous;
    std::vector<double> cuts{0, 1};
    auto add = [&](double t) {
        if (t > 0 && t < 1) {
            m2::require(cuts.size() < 400000, "Terrain trace work budget exceeded");
            cuts.push_back(t);
        }
    };
    const double minX = std::min(x0, x1), maxX = std::max(x0, x1), minY = std::min(y0, y1),
                 maxY = std::max(y0, y1);
    for (uint32_t i = 0; i < 256; ++i) {
        const auto &chunk = chunks_[i];
        if (maxX < chunk.x[8] || minX > chunk.x[0] || maxY < chunk.y[8] || minY > chunk.y[0])
            continue;
        for (uint32_t row = 0; row < 8; ++row)
            for (uint32_t col = 0; col < 8; ++col) {
                if (maxX < chunk.x[row + 1] || minX > chunk.x[row] || maxY < chunk.y[col + 1] ||
                    minY > chunk.y[col])
                    continue;
                const auto first = chunk.cells[row * 8 + col];
                if (first == UINT32_MAX)
                    continue;
                for (uint32_t triangle = 0; triangle < 4; ++triangle)
                    for (uint32_t edge = 0; edge < 3; ++edge) {
                        const auto at = first + triangle * 3;
                        const auto &a = geometry_->vertices[geometry_->indices[at + edge]].position;
                        const auto &b =
                            geometry_->vertices[geometry_->indices[at + (edge + 1) % 3]].position;
                        const double ex = double(b.x) - a.x, ey = double(b.y) - a.y;
                        const double ax = double(a.x) - x0, ay = double(a.y) - y0,
                                     denominator = dx * ey - dy * ex;
                        if (denominator != 0) {
                            const double t = (ax * ey - ay * ex) / denominator,
                                         u = (ax * dy - ay * dx) / denominator;
                            if (u >= -1e-12 && u <= 1 + 1e-12)
                                add(t);
                        } else if (ax * dy - ay * dx == 0) {
                            if (std::abs(dx) >= std::abs(dy)) {
                                add(ax / dx);
                                add((double(b.x) - x0) / dx);
                            } else {
                                add(ay / dy);
                                add((double(b.y) - y0) / dy);
                            }
                        }
                    }
            }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end(),
                           [](double a, double b) { return std::abs(a - b) <= 1e-12; }),
               cuts.end());
    auto inspect = [&](double t) {
        auto hit = heightAt(x0 + dx * t, y0 + dy * t);
        if (!hit || hit->normal.z < minNormal || hit->height - previous->height > maxRise + 1e-8 ||
            previous->height - hit->height > maxDrop + 1e-8)
            return false;
        previous = hit;
        return true;
    };
    for (size_t i = 1; i < cuts.size(); ++i)
        if (!inspect((cuts[i - 1] + cuts[i]) * .5) || !inspect(cuts[i]))
            return std::nullopt;
    if (!inspect(1))
        return std::nullopt;
    return previous;
}
std::optional<TerrainSurfaceHit> TerrainSurface::heightAt(double x, double y) const {
    return query(x, y, -std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::infinity(), false);
}
std::optional<TerrainSurfaceHit> TerrainSurface::raycastDown(double x, double y, double z,
                                                             double distance) const {
    m2::require(std::isfinite(z) && std::isfinite(distance) && distance >= 0,
                "Invalid terrain downward ray");
    return query(x, y, z - distance, z, true);
}
std::optional<TerrainSurfaceHit> TerrainSurface::query(double x, double y, double low, double high,
                                                       bool ray) const {
    m2::require(std::isfinite(x) && std::isfinite(y), "Invalid terrain surface coordinates");
    std::optional<TerrainSurfaceHit> hit;
    for (uint32_t i = 0; i < 256; ++i) {
        const auto &chunk = chunks_[i];
        if (x > chunk.x[0] || x < chunk.x[8] || y > chunk.y[0] || y < chunk.y[8])
            continue;
        // Inclusive edges can belong to adjacent cells/chunks. Inspect all
        // candidates, allowing a valid neighbor to meet the edge of a hole.
        for (uint32_t row = 0; row < 8; ++row) {
            if (x > chunk.x[row] || x < chunk.x[row + 1])
                continue;
            for (uint32_t col = 0; col < 8; ++col) {
                if (y > chunk.y[col] || y < chunk.y[col + 1])
                    continue;
                const auto first = chunk.cells[row * 8 + col];
                if (first == UINT32_MAX)
                    continue;
                for (uint32_t triangle = 0; triangle < 4; ++triangle) {
                    const auto at = first + triangle * 3;
                    const auto &a = geometry_->vertices[geometry_->indices[at]].position;
                    const auto &b = geometry_->vertices[geometry_->indices[at + 1]].position;
                    const auto &c = geometry_->vertices[geometry_->indices[at + 2]].position;
                    const double ux = double(b.x) - a.x, uy = double(b.y) - a.y,
                                 uz = double(b.z) - a.z;
                    const double vx = double(c.x) - a.x, vy = double(c.y) - a.y,
                                 vz = double(c.z) - a.z;
                    const double determinant = ux * vy - uy * vx;
                    m2::require(determinant > 0 && std::isfinite(determinant),
                                "Degenerate terrain surface triangle");
                    double wb = ((x - a.x) * vy - (y - a.y) * vx) / determinant;
                    double wc = (ux * (y - a.y) - uy * (x - a.x)) / determinant, wa = 1 - wb - wc;
                    constexpr double edge = 1e-12;
                    if (wa < -edge || wb < -edge || wc < -edge)
                        continue;
                    wa = std::max(0.0, wa);
                    wb = std::max(0.0, wb);
                    wc = std::max(0.0, wc);
                    const double sum = wa + wb + wc;
                    const double height = (wa * a.z + wb * b.z + wc * c.z) / sum;
                    const double tolerance = 64 * std::numeric_limits<double>::epsilon() *
                                             std::max(1.0, std::abs(height));
                    if (height < low - tolerance || height > high + tolerance ||
                        (hit && height <= hit->height))
                        continue;
                    const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = determinant;
                    const double length = std::hypot(nx, ny, nz);
                    hit = TerrainSurfaceHit{
                        height,
                        ray ? std::max(0.0, high - height) : 0,
                        {float(nx / length), float(ny / length), float(nz / length)},
                        i,
                        row * 8 + col,
                        triangle,
                        geometry_->chunks[i].area};
                }
            }
        }
    }
    return hit;
}
} // namespace wow::android
