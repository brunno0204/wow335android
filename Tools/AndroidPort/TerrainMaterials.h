#pragma once
#include "TerrainGeometry.h"
#include "TextureDecoder.h"
#include <memory>
namespace wow::android {
struct TerrainPreviewPass {
    int32_t texture = -1, mask = -1;
};
struct TerrainPreviewMaterial {
    int32_t texture = -1;
    std::string fallback;
    std::vector<TerrainPreviewPass> passes;
    bool additive = false;
};
struct TerrainPreviewMaterials {
    std::vector<std::shared_ptr<const TexturePixels>> textures;
    std::vector<TerrainPreviewMaterial> chunks;
    std::vector<uint8_t> clamp;
    size_t texturedChunks() const noexcept;
};
bool terrainNeedsAlphaMode(const TerrainGeometry &geometry);
TerrainPreviewMaterials prepareTerrainMaterials(const TerrainGeometry &geometry);
} // namespace wow::android
