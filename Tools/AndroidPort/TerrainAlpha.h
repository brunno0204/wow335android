#pragma once
#include "TerrainGeometry.h"
namespace wow::android {
TerrainAlphaMode decodeTerrainAlphaMode(const std::vector<uint8_t> &wdt, uint32_t tileX,
                                        uint32_t tileY);
TerrainAlphaMode loadTerrainAlphaMode(const std::string &map, uint32_t tileX, uint32_t tileY);
// Fixed 64x64. Legacy low/high nibbles; normal legacy mode fixes the last row/column.
// Big8 RLE runs are bounded to the original 64-byte row buffer.
std::vector<uint8_t> decodeTerrainAlpha(const std::vector<uint8_t> &data, size_t offset, size_t end,
                                        TerrainAlphaMode mode, bool compressed,
                                        bool preserveLegacyBorder);
} // namespace wow::android
