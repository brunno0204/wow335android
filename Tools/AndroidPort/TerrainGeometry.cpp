#include "TerrainGeometry.h"
#include "M2File.h"
#include "WoW/Source/WorldClient/WorldConstants.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
namespace wow::android {
namespace {
constexpr uint32_t MVER = 0x4d564552, MHDR = 0x4d484452, MCIN = 0x4d43494e, MCNK = 0x4d434e4b,
                   MCVT = 0x4d435654, MCNR = 0x4d434e52, MTEX = 0x4d544558, MCLY = 0x4d434c59,
                   MTXF = 0x4d545846, MCAL = 0x4d43414c;
struct Chunk {
    size_t start, data, end;
    uint32_t tag;
};
Chunk chunk(const std::vector<uint8_t> &b, size_t at, size_t end) {
    m2::require(at <= end && end - at >= 8, "Truncated ADT chunk header");
    const auto size = m2::u32(b, at + 4);
    m2::require(size <= end - at - 8, "ADT chunk exceeds parent");
    return {at, at + 8, at + 8 + size, m2::u32(b, at)};
}
float number(const std::vector<uint8_t> &b, size_t at) {
    const uint32_t bits = m2::u32(b, at);
    float v;
    std::memcpy(&v, &bits, 4);
    m2::require(std::isfinite(v), "Nonfinite ADT terrain value");
    return v;
}
Chunk inner(const std::vector<uint8_t> &b, const Chunk &parent, uint32_t offset, uint32_t tag) {
    m2::require(offset >= 136 && offset <= parent.end - parent.start,
                "Invalid ADT terrain subchunk offset");
    auto c = chunk(b, parent.start + offset, parent.end);
    m2::require(c.tag == tag, "Invalid ADT terrain subchunk token");
    return c;
}
void tile(uint32_t x, uint32_t y) { m2::require(x < 64 && y < 64, "Invalid ADT tile coordinates"); }
} // namespace
std::string terrainTilePath(const std::string &map, uint32_t x, uint32_t y) {
    tile(x, y);
    m2::require(!map.empty() && map.size() <= 128, "Invalid terrain map name");
    for (unsigned char c : map)
        m2::require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_',
                    "Invalid terrain map name");
    return "World\\Maps\\" + map + "\\" + map + "_" + std::to_string(x) + "_" + std::to_string(y) +
           ".adt";
}
TerrainGeometry loadTerrainGeometry(const std::string &path, uint32_t x, uint32_t y,
                                    bool includeMaterials) {
    tile(x, y);
    return decodeTerrainGeometry(m2::readFile(path, 64 * 1024 * 1024), x, y, includeMaterials);
}
TerrainGeometry decodeTerrainGeometry(const std::vector<uint8_t> &b, uint32_t tileX, uint32_t tileY,
                                      bool includeMaterials) {
    tile(tileX, tileY);
    m2::require(b.size() >= 12 && b.size() <= 64 * 1024 * 1024, "Invalid ADT file size");
    const auto version = chunk(b, 0, b.size());
    m2::require(version.tag == MVER && version.end - version.data == 4 &&
                    m2::u32(b, version.data) == 18,
                "Unsupported ADT version");
    const auto header = chunk(b, version.end, b.size());
    m2::require(header.tag == MHDR && header.end - header.data == 64, "Invalid ADT header");
    std::map<size_t, Chunk> terrain;
    Chunk index{};
    Chunk textures{}, textureFlags{};
    bool haveTextures = false, haveTextureFlags = false;
    bool haveIndex = false;
    size_t work = 0;
    for (size_t at = header.end; at < b.size();) {
        m2::require(++work <= 4096, "ADT top-level chunk budget exceeded");
        const auto c = chunk(b, at, b.size());
        m2::require(c.tag != MVER && c.tag != MHDR, "Duplicate ADT header");
        if (c.tag == MCIN) {
            m2::require(!haveIndex && c.end - c.data == 256 * 16, "Invalid ADT chunk index");
            index = c;
            haveIndex = true;
        } else if (includeMaterials && c.tag == MTEX) {
            m2::require(!haveTextures && c.end - c.data <= 1024 * 1024,
                        "Invalid ADT texture names");
            textures = c;
            haveTextures = true;
        } else if (includeMaterials && c.tag == MTXF) {
            m2::require(!haveTextureFlags, "Duplicate ADT texture flags");
            textureFlags = c;
            haveTextureFlags = true;
        } else if (c.tag == MCNK) {
            m2::require(terrain.size() < 256 && c.end - c.data >= 128, "Invalid ADT terrain chunk");
            terrain.emplace(c.start, c);
        }
        at = c.end;
    }
    m2::require(haveIndex && terrain.size() == 256, "ADT requires 256 terrain chunks");
    const auto relativeIndex = m2::u32(b, header.data + 4);
    m2::require(relativeIndex == index.start - header.data, "ADT header/index mismatch");
    TerrainGeometry out;
    out.tileX = tileX;
    out.tileY = tileY;
    if (includeMaterials)
        out.alphaMip = b[header.data + 0x30];
    if (haveTextures) {
        m2::require(m2::u32(b, header.data + 8) == textures.start - header.data,
                    "ADT texture names pointer mismatch");
        for (size_t at = textures.data; at < textures.end;) {
            size_t end = at;
            while (end < textures.end && b[end])
                ++end;
            m2::require(end < textures.end && end > at && end - at <= 4096 &&
                            out.texturePaths.size() < 256,
                        "Invalid ADT texture filename");
            out.texturePaths.emplace_back(reinterpret_cast<const char *>(b.data() + at), end - at);
            at = end + 1;
        }
    } else if (includeMaterials) {
        m2::require(m2::u32(b, header.data + 8) == 0, "Missing ADT texture names");
    }
    out.textureFlags.resize(out.texturePaths.size());
    if (haveTextureFlags) {
        m2::require(m2::u32(b, header.data + 0x2c) == textureFlags.start - header.data &&
                        textureFlags.end - textureFlags.data == out.texturePaths.size() * 4,
                    "Invalid ADT texture flags table");
        for (size_t i = 0; i < out.textureFlags.size(); ++i)
            out.textureFlags[i] = m2::u32(b, textureFlags.data + 4 * i);
    } else if (includeMaterials) {
        m2::require(m2::u32(b, header.data + 0x2c) == 0, "Missing ADT texture flags");
    }
    out.vertices.reserve(256 * 145);
    out.indices.reserve(256 * 768);
    out.chunks.reserve(256);
    std::array<bool, 256> visited{};
    for (uint32_t i = 0; i < 256; ++i) {
        const auto found = terrain.find(m2::u32(b, index.data + 16 * i));
        m2::require(found != terrain.end(), "ADT index does not reference a terrain chunk");
        const auto &c = found->second;
        const size_t h = c.data;
        const auto x = m2::u32(b, h + 4), y = m2::u32(b, h + 8);
        m2::require(x < 16 && y < 16 && i == y * 16 + x && !visited[i],
                    "ADT index/coordinate mismatch");
        visited[i] = true;
        const auto heights = inner(b, c, m2::u32(b, h + 0x14), MCVT);
        const auto normals = inner(b, c, m2::u32(b, h + 0x18), MCNR);
        m2::require(
            heights.end - heights.data == 145 * 4 &&
                (normals.end - normals.data == 145 * 3 || normals.end - normals.data == 448),
            "Invalid ADT terrain sample size");
        m2::require(heights.end <= normals.start || normals.end <= heights.start,
                    "Overlapping ADT terrain samples");
        const float baseZ = number(b, h + 0x70);
        // CMapChunk::Create derives XY from global chunk indices, not stored header XY.
        const float top = HALF_WORLD_SIZE - float(tileY * 16 + y) * 33.333332f;
        const float left = HALF_WORLD_SIZE - float(tileX * 16 + x) * 33.333332f;
        TerrainChunk result{x,
                            y,
                            m2::u32(b, h + 0x34),
                            m2::u32(b, h),
                            m2::u16(b, h + 0x3c),
                            uint32_t(out.vertices.size()),
                            uint32_t(out.indices.size()),
                            0};
        for (int k = 0; k < 8; ++k) {
            result.lowQualityTexMap[size_t(k)] = m2::u16(b, h + 0x40 + 2 * size_t(k));
            result.noEffectDoodad[size_t(k)] = b[h + 0x50 + size_t(k)];
        }
        if (includeMaterials) {
            const auto count = m2::u32(b, h + 0xc);
            m2::require(count <= 4, "Too many ADT material layers");
            if (count) {
                const auto layers = inner(b, c, m2::u32(b, h + 0x1c), MCLY);
                m2::require(layers.end - layers.data == count * 16 &&
                                (layers.end <= heights.start || heights.end <= layers.start) &&
                                (layers.end <= normals.start || normals.end <= layers.start),
                            "Invalid ADT layer records");
                for (uint32_t j = 0; j < count; ++j) {
                    const auto at = layers.data + 16 * j;
                    TerrainLayer layer{m2::u32(b, at), m2::u32(b, at + 4), m2::u32(b, at + 8),
                                       m2::u32(b, at + 12)};
                    m2::require(layer.texture < out.texturePaths.size(),
                                "ADT layer texture exceeds names");
                    result.layers.push_back(layer);
                }
                if (std::any_of(result.layers.begin(), result.layers.end(),
                                [](const auto &layer) { return (layer.flags & 0x100) != 0; })) {
                    const auto alpha = inner(b, c, m2::u32(b, h + 0x24), MCAL);
                    m2::require(alpha.end - alpha.start == m2::u32(b, h + 0x28) &&
                                    alpha.end - alpha.data <= 65536 &&
                                    (alpha.end <= heights.start || heights.end <= alpha.start) &&
                                    (alpha.end <= normals.start || normals.end <= alpha.start) &&
                                    (alpha.end <= layers.start || layers.end <= alpha.start),
                                "Invalid ADT alpha range");
                    result.alpha.assign(b.begin() + alpha.data, b.begin() + alpha.end);
                    for (const auto &layer : result.layers)
                        if (layer.flags & 0x100)
                            m2::require(layer.alphaOffset < result.alpha.size(),
                                        "ADT alpha offset exceeds MCAL");
                }
            }
        }
        uint32_t sample = 0;
        for (uint32_t row = 0; row < 9; ++row) {
            auto emit = [&](uint32_t col, bool center) {
                const float half = center ? HALF_SMOLTILE_SIZE : 0;
                const float z = baseZ + number(b, heights.data + 4 * sample);
                m2::require(std::isfinite(z), "ADT terrain height overflow");
                auto normal = [&](unsigned axis) {
                    const uint8_t packed = b[normals.data + 3 * sample + axis];
                    const int value = packed < 128 ? packed : int(packed) - 256;
                    return float(value) * 0.0078740157f;
                };
                out.vertices.push_back(
                    {{top - float(row) * SMOLTILE_SIZE - half,
                      left - float(col) * SMOLTILE_SIZE - half, z},
                     {normal(0), normal(1), normal(2)},
                     {float(col) + (center ? .5f : 0), float(row) + (center ? .5f : 0)},
                     {(float(col) + (center ? .5f : 0)) * .125f,
                      (float(row) + (center ? .5f : 0)) * .125f}});
                ++sample;
            };
            for (uint32_t col = 0; col < 9; ++col)
                emit(col, false);
            if (row < 8)
                for (uint32_t col = 0; col < 8; ++col)
                    emit(col, true);
        }
        // Literal full-detail triangle order from CMapChunk::CreateIndices.
        for (uint32_t row = 0; row < 8; ++row)
            for (uint32_t col = 0; col < 8; ++col) {
                if (result.holes & (1u << ((row / 2) * 4 + col / 2)))
                    continue;
                const uint32_t base = result.firstVertex + row * 17 + col;
                for (uint32_t local : {9u, 0u, 17u, 9u, 1u, 0u, 9u, 18u, 1u, 9u, 17u, 18u})
                    out.indices.push_back(uint16_t(base + local));
            }
        result.indexCount = uint32_t(out.indices.size()) - result.firstIndex;
        out.chunks.push_back(result);
    }
    // MH2O (MHDR +0x28): liquids. Malformed liquid data is skipped, never fatal.
    try {
        const uint32_t water = m2::u32(b, header.data + 0x28);
        if (water) {
            const auto c = chunk(b, header.data + water, b.size());
            if (c.tag == 0x4d48324f && c.end - c.data >= 256 * 12) {
                for (uint32_t i = 0; i < 256; ++i) {
                    const size_t entry = c.data + 12 * i;
                    const uint32_t info = m2::u32(b, entry), layers = m2::u32(b, entry + 4);
                    if (!info || !layers || layers > 8)
                        continue;
                    const uint32_t cx = i % 16, cy = i / 16;
                    const float top = HALF_WORLD_SIZE - float(tileY * 16 + cy) * 33.333332f;
                    const float left = HALF_WORLD_SIZE - float(tileX * 16 + cx) * 33.333332f;
                    for (uint32_t layer = 0; layer < layers; ++layer) {
                        const size_t at = c.data + info + 24 * layer;
                        m2::require(at + 24 <= c.end, "MH2O instance outside the chunk");
                        TerrainLiquid liquid;
                        liquid.chunk = i;
                        liquid.type = m2::u16(b, at);
                        const float minHeight = number(b, at + 4);
                        liquid.xOffset = b[at + 12];
                        liquid.yOffset = b[at + 13];
                        liquid.width = b[at + 14];
                        liquid.height = b[at + 15];
                        if (!liquid.width || !liquid.height || liquid.xOffset + liquid.width > 8 ||
                            liquid.yOffset + liquid.height > 8)
                            continue;
                        liquid.top = top;
                        liquid.left = left;
                        const uint32_t cells = uint32_t(liquid.width) * liquid.height;
                        const uint32_t corners = uint32_t(liquid.width + 1) * (liquid.height + 1);
                        const uint32_t existsAt = m2::u32(b, at + 16), vertexAt = m2::u32(b, at + 20);
                        liquid.exists.assign(cells, 1);
                        if (existsAt) {
                            for (uint32_t k = 0; k < cells; ++k) {
                                const size_t byte = c.data + existsAt + k / 8;
                                m2::require(byte < c.end, "MH2O bitmap outside the chunk");
                                liquid.exists[k] = (b[byte] >> (k % 8)) & 1;
                            }
                        }
                        liquid.heights.assign(corners, minHeight);
                        // Ocean (2) has depth only; other liquids store a height per corner.
                        if (vertexAt && liquid.type != 2 && liquid.type != 14) {
                            m2::require(c.data + vertexAt + 4 * corners <= c.end, "MH2O heights outside the chunk");
                            for (uint32_t k = 0; k < corners; ++k) {
                                const float hgt = number(b, c.data + vertexAt + 4 * k);
                                liquid.heights[k] = std::abs(hgt) < 1e5f ? hgt : minHeight;
                            }
                        }
                        out.liquids.push_back(std::move(liquid));
                    }
                }
            }
        }
    } catch (const std::exception &) {
        out.liquids.clear();
    }
    return out;
}
} // namespace wow::android
