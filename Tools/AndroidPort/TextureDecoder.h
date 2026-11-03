#pragma once
#include <cstdint>
#include <vector>

namespace wow::android {
struct TexturePixels {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
// Bounded BLP2 parser, using the reconstructed Gx DXT decoder. Output is portable
// RGBA, so Android GPUs do not need desktop S3TC compressed-texture extensions.
TexturePixels decodeBlp2(const std::vector<uint8_t>& file, uint32_t mip = 0);
}
