#include "M2PreviewDrawOrder.h"
#include "M2File.h"
#include <algorithm>
#include <cmath>
namespace wow::android {
std::vector<size_t> m2PreviewDrawOrder(const M2PreviewMaterials &materials,
                                       const M2Geometry &geometry,
                                       const std::vector<M2VertexPNT2> &vertices,
                                       const C44Matrix &projection,
                                       const std::vector<uint8_t> *visibleSections) {
    m2::require(!visibleSections || visibleSections->size() == geometry.sections.size(),
                "Invalid M2 preview visibility count");
    m2::require(vertices.size() == geometry.vertices.size(), "Invalid M2 preview sort vertices");
    const double column[]{projection.a2, projection.b2, projection.c2, projection.d2};
    for (double v : column)
        m2::require(std::isfinite(v), "Nonfinite M2 preview sort projection");
    std::vector<size_t> order, transparent;
    std::vector<double> depths(geometry.sections.size());
    std::vector<bool> sampled(geometry.sections.size());
    uint64_t work = 0;
    for (size_t i = 0; i < materials.batches.size(); ++i) {
        const auto &batch = materials.batches[i];
        m2::require(batch.section < geometry.sections.size(), "Invalid M2 preview sort section");
        if (visibleSections && !(*visibleSections)[batch.section])
            continue;
        if (!batch.textured() || batch.blend <= 1) {
            order.push_back(i);
            continue;
        }
        transparent.push_back(i);
        if (sampled[batch.section])
            continue;
        sampled[batch.section] = true;
        const auto &section = geometry.sections[batch.section];
        const size_t first = section.m_vertexStart, count = section.m_vertexCount;
        m2::require(first <= vertices.size() && count <= vertices.size() - first,
                    "Invalid M2 preview sort vertex range");
        work += count;
        m2::require(work <= 4 * 1024 * 1024, "M2 preview sort work budget exceeded");
        double sum = 0;
        for (size_t j = first; j < first + count; ++j) {
            const auto &p = vertices[j].m_position;
            const double depth = double(p.x) * column[0] + double(p.y) * column[1] +
                                 double(p.z) * column[2] + column[3];
            m2::require(std::isfinite(depth), "Nonfinite M2 preview sort depth");
            sum += depth;
        }
        depths[batch.section] = count ? sum / count : 0;
    }
    std::stable_sort(transparent.begin(), transparent.end(), [&](size_t a, size_t b) {
        return depths[materials.batches[a].section] > depths[materials.batches[b].section];
    });
    order.insert(order.end(), transparent.begin(), transparent.end());
    return order;
}
} // namespace wow::android
