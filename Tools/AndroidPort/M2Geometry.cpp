#include "M2Geometry.h"
#include "M2File.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace wow::android {
namespace {
static_assert(sizeof(M2Vertex) == 48 && sizeof(M2SkinSection) == 48 && sizeof(M2CompBone) == 88);
static_assert(offsetof(M2Data, m_vertices) == 0x3c && offsetof(M2Data, m_boneCombos) == 0x78);
#if defined(__BYTE_ORDER__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Native M2 geometry requires little endian");
#endif
void require(bool condition, const char *message) {
    if (!condition)
        throw std::invalid_argument(message);
}
using m2::u32;
using m2::array;
void finite(const M2Vertex &v) {
    const float values[]{v.m_position.x,    v.m_position.y,   v.m_position.z,    v.m_normal.x,
                         v.m_normal.y,      v.m_normal.z,     v.m_texcoord[0].x, v.m_texcoord[0].y,
                         v.m_texcoord[1].x, v.m_texcoord[1].y};
    for (float value : values)
        require(std::isfinite(value), "Nonfinite M2 geometry");
}
} // namespace
M2Geometry decodeM2Geometry(const std::vector<uint8_t> &model, const std::vector<uint8_t> &skin) {
    constexpr size_t skinHeader = 48;
    const size_t modelHeader=m2::modelHeader(model);
    require(skin.size() >= skinHeader && skin.size() <= 64 * 1024 * 1024, "Invalid SKIN geometry file size");
    require(u32(skin, 0) == 0x4e494b53, "Invalid SKIN magic");
    require(u32(model, 0x44) > 0 && u32(model, 0x44) <= 4, "Invalid M2 skin profile count");
    auto source = array<M2Vertex>(model, 0x3c, 1024 * 1024, modelHeader);
    // Check the bone table range, but do not interpret/evaluate animation tracks.
    const auto bones = array<M2CompBone>(model, 0x2c, 65536, modelHeader);
    const auto combos = array<uint16_t>(model, 0x78, 1024 * 1024, modelHeader);
    const auto lookup = array<uint16_t>(skin, 4, 65536, skinHeader);
    const auto boneLookup = array<ubyte4>(skin, 20, 65536, skinHeader);
    M2Geometry out;
    out.boneCount = bones.size();
    out.indices = array<uint16_t>(skin, 12, 4 * 1024 * 1024, skinHeader);
    out.sections = array<M2SkinSection>(skin, 28, 65536, skinHeader);
    require(boneLookup.size() == lookup.size(), "SKIN bone/vertex table mismatch");
    require(out.indices.size() % 3 == 0, "Incomplete SKIN triangle");
    out.vertices.reserve(lookup.size());
    out.boneIndices.assign(lookup.size(), {0, 0, 0, 0});
    for (uint16_t index : lookup) {
        require(index < source.size(), "SKIN vertex lookup exceeds M2 vertices");
        finite(source[index]);
        out.vertices.push_back(source[index]);
    }
    for (uint16_t index : out.indices)
        require(index < lookup.size(), "SKIN triangle index exceeds vertices");
    std::vector<bool> prepared(lookup.size());
    uint64_t vertexWork = 0, indexWork = 0;
    for (const auto &section : out.sections) {
        const uint32_t first = section.m_vertexStart, count = section.m_vertexCount;
        const uint32_t start = section.m_indexStart, indices = section.m_indexCount;
        vertexWork += count;
        indexWork += indices;
        require(vertexWork <= 4 * 1024 * 1024 && indexWork <= 16 * 1024 * 1024,
                "SKIN section work budget exceeded");
        require(first <= lookup.size() && count <= lookup.size() - first, "SKIN section vertex range");
        require(start <= out.indices.size() && indices <= out.indices.size() - start && indices % 3 == 0,
                "SKIN section triangle range");
        require(section.m_boneInfluences >= 1 && section.m_boneInfluences <= 4 && section.m_boneCount > 0,
                "Unsupported SKIN bone influence count");
        for (uint32_t i = start; i < start + indices; ++i)
            require(out.indices[i] >= first && out.indices[i] < first + count,
                    "SKIN triangle escapes section vertices");
        for (uint32_t i = first; i < first + count; ++i) {
            auto vertex = source[lookup[i]];
            bool ended = false;
            unsigned active = 0;
            for (unsigned k = 0; k < 4; ++k) {
                if (!vertex.m_weights.b[k]) {
                    ended = true;
                    continue;
                }
                require(!ended && k < section.m_boneInfluences, "Invalid SKIN packed influences");
                ++active;
                uint32_t local = boneLookup[i].b[k], combo = uint32_t(section.m_boneComboIndex) + local;
                require(local < section.m_boneCount && combo < combos.size(),
                        "SKIN palette lookup exceeds bone combos");
                require(combos[combo] < out.boneCount, "SKIN global bone index exceeds the bones");
                vertex.m_indices.b[k] = uint8_t(combos[combo]);
                if (combos[combo] > 255)
                    out.wideBones = true;
                if (prepared[i])
                    require(out.boneIndices[i][k] == combos[combo], "Conflicting shared SKIN bone palettes");
                out.boneIndices[i][k] = combos[combo];
            }
            require(active > 0, "Unweighted SKIN section vertex");
            out.vertices[i] = vertex;
            prepared[i] = true;
        }
    }
    return out;
}
M2Geometry loadM2Geometry(const std::string &modelPath, const std::string &skinPath) {
    const auto model = m2::readFile(modelPath, 128 * 1024 * 1024);
    const auto skin = m2::readFile(skinPath, 64 * 1024 * 1024);
    return decodeM2Geometry(model, skin);
}
} // namespace wow::android
