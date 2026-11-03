#pragma once
#include "TextureDecoder.h"
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace wow::android {
using M2TextureReplacements = std::map<uint32_t, std::string>;
struct M2PreviewTextureSource {
    uint32_t type = 0, flags = 0;
    std::string path; // Named type-0 path; replaceable types require a binding.
};
struct M2PreviewMaterialSource;
struct M2PreviewTexture {
    std::string path;
    uint32_t flags = 0;
    std::shared_ptr<const TexturePixels> pixels;
    uint32_t type = 0;
};
struct M2PreviewBatch {
    uint16_t section = 0, materialFlags = 0, blend = 0, uv = 0;
    int32_t texture = -1, texture2 = -1;
    uint16_t stages = 0, uv2 = 1;
    uint16_t combiner[2]{0, 0}; // Original M2COMBINER codes, not Gx op codes.
    // Animated material references (0xffff none) for M2EffectsData.
    uint16_t colorIndex = 0xffff, weightIndex = 0xffff, transformIndex = 0xffff;
    float alpha = 1; // static texture weight x color alpha (sequence 0, first key)
    bool textured() const noexcept {
        return fallback.empty() && texture >= 0 && stages >= 1 && stages <= 2 &&
               (stages == 1 || texture2 >= 0);
    }
    std::string fallback;
};
struct M2PreviewBatchSource {
    M2PreviewBatch pass;
    uint16_t textures[2]{};
};
struct M2PreviewMaterialSource {
    std::vector<M2PreviewTextureSource> textures;
    std::vector<M2PreviewBatchSource> batches;
};
struct M2PreviewMaterials {
    std::shared_ptr<const M2PreviewMaterialSource> source;
    std::vector<M2PreviewTexture> textures;
    std::vector<M2PreviewBatch> batches;
    size_t texturedBatches() const noexcept;
};
// DEVELOPMENT subset: one/two named BLP2s, seven blend modes, UV0/UV1. Not full
// M2 material evaluation. Uses the SAME snapshots as validated geometry. Bad
// references/missing supported textures reject; valid unsupported batches have
// explicit gray fallback reasons. Owned data needs no frame-time archive IO.
// Bounds: 256 texture records; 128 MiB each for encoded bytes, unique decoded pixels
// and logical texture upload footprint per load.
M2PreviewMaterials loadM2PreviewMaterials(const std::vector<uint8_t> &model,
                                          const std::vector<uint8_t> &skin,
                                          const M2TextureReplacements &replacements = {});
// Pure bounded recipe decode; no BLP reads. Prepare only immutable recipes on
// an IO thread, then install the completed materials on the frame thread.
M2PreviewMaterialSource decodeM2PreviewMaterialSource(const std::vector<uint8_t> &model,
                                                      const std::vector<uint8_t> &skin);
M2PreviewMaterials prepareM2PreviewMaterials(std::shared_ptr<const M2PreviewMaterialSource> source,
                                             const M2TextureReplacements &replacements = {});
// Development format: decimal-type=archive-path;decimal-type=archive-path.
// Empty clears bindings. Up to 32 unique types, 64KiB total, 4096 bytes/path.
M2TextureReplacements parseM2TextureReplacements(const std::string &text);
} // namespace wow::android
