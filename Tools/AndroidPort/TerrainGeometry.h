#pragma once
#include "Engine/Source/Tempest/C2Vector.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace wow::android {
enum class TerrainAlphaMode { Unknown, Legacy4, Big8 };
struct TerrainVertex {
    C3Vector position, normal;
    C2Vector uv;
    C2Vector maskUv;
};
struct TerrainLayer {
    uint32_t texture = 0, flags = 0, alphaOffset = 0, effect = 0;
};
struct TerrainChunk {
    uint32_t x = 0, y = 0, area = 0, flags = 0;
    uint16_t holes = 0;
    uint32_t firstVertex = 0, firstIndex = 0, indexCount = 0;
    std::vector<TerrainLayer> layers;
    std::vector<uint8_t> alpha;
    // MCNK 0x40: 2-bit dominant layer per cell (row-major 8x8); 0x50: no detail doodads bit.
    std::array<uint16_t, 8> lowQualityTexMap{};
    std::array<uint8_t, 8> noEffectDoodad{};
};
// One MH2O liquid instance of a chunk (3.3.5a): cells xOffset..+width, yOffset..+height of the
// 8x8 chunk grid, heights per corner ((width+1)*(height+1)), cells present by bitmap.
struct TerrainLiquid {
    uint32_t chunk = 0;
    uint16_t type = 0; // LiquidType.dbc (1 water, 2 ocean, 3 magma, 4 slime, ...)
    uint8_t xOffset = 0, yOffset = 0, width = 0, height = 0;
    float top = 0, left = 0; // chunk corner (world X, Y)
    std::vector<float> heights;
    std::vector<uint8_t> exists; // width*height, 1 = liquid
};
// Checked WoW 3.3.5a version-18 ADT terrain subset. Optional material metadata;
// no texture pixels, liquids or objects are stored here.
struct TerrainGeometry {
    uint32_t tileX = 0, tileY = 0;
    std::vector<TerrainVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<TerrainChunk> chunks;
    std::vector<std::string> texturePaths;
    std::vector<uint32_t> textureFlags;
    TerrainAlphaMode alphaMode = TerrainAlphaMode::Unknown;
    uint8_t alphaMip = 0;
    std::vector<TerrainLiquid> liquids;
};
TerrainGeometry decodeTerrainGeometry(const std::vector<uint8_t> &bytes, uint32_t tileX,
                                      uint32_t tileY, bool includeMaterials = false);
TerrainGeometry loadTerrainGeometry(const std::string &path, uint32_t tileX, uint32_t tileY,
                                    bool includeMaterials = false);
// Canonical archive path; ASCII map directory name only, no traversal/separators.
std::string terrainTilePath(const std::string &map, uint32_t tileX, uint32_t tileY);
} // namespace wow::android
