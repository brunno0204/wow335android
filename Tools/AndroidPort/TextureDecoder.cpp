#include "TextureDecoder.h"
#include "Engine/Source/Gx/Blit.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace wow::android {
namespace {
uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void need(bool valid) { if (!valid) throw std::runtime_error("Malformed or unsupported BLP2 texture"); }
}
TexturePixels decodeBlp2(const std::vector<uint8_t>& file, uint32_t mip) {
    need(file.size() >= 148 && mip < 16);
    const auto* bytes = file.data();
    need(u32(bytes) == 0x32504c42 && u32(bytes + 4) == 1);
    const auto baseWidth = u32(bytes + 12), baseHeight = u32(bytes + 16);
    need(baseWidth && baseHeight && baseWidth <= 8192 && baseHeight <= 8192);
    TexturePixels result;
    result.width = std::max(1u, baseWidth >> mip);
    result.height = std::max(1u, baseHeight >> mip);
    const uint64_t count = uint64_t(result.width) * result.height;
    need(count <= 16 * 1024 * 1024);
    const auto offset = u32(bytes + 20 + mip * 4), size = u32(bytes + 84 + mip * 4);
    need(offset >= 148 && offset <= file.size() && size <= file.size() - offset && size);
    const auto* source = bytes + offset;
    switch (bytes[8]) {
    case 1: { // palette plus packed alpha
        const unsigned bits = bytes[9];
        need(bits == 0 || bits == 1 || bits == 4 || bits == 8);
        need(file.size() >= 1172 && offset >= 1172 && size >= count + (count * bits + 7) / 8);
        result.rgba.resize(static_cast<size_t>(count) * 4);
        for (size_t i = 0; i < count; ++i) {
            const auto* color = bytes + 148 + size_t(source[i]) * 4;
            uint8_t alpha = 255;
            if (bits == 1) alpha = ((source[count + i / 8] >> (i % 8)) & 1) * 255;
            if (bits == 4) alpha = ((source[count + i / 2] >> ((i % 2) * 4)) & 15) * 17;
            if (bits == 8) alpha = source[count + i];
            result.rgba[i * 4] = color[2]; result.rgba[i * 4 + 1] = color[1];
            result.rgba[i * 4 + 2] = color[0]; result.rgba[i * 4 + 3] = alpha;
        }
        break;
    }
    case 2: { // DXT1, DXT3 or DXT5
        need(bytes[10] == 0 || bytes[10] == 1 || bytes[10] == 7);
        const unsigned blockBytes = bytes[10] == 0 ? 8 : 16;
        const unsigned blocksAcross = result.width == 6 * result.height ? 6 * ((result.height + 3) / 4) : (result.width + 3) / 4;
        const uint64_t required = uint64_t(blocksAcross) * ((result.height + 3) / 4) * blockBytes;
        need(size >= required);
        result.rgba.resize(static_cast<size_t>(count) * 4);
        // Align potentially unaligned on-disk mip offsets for the engine decoder.
        std::vector<uint32_t> blocks(static_cast<size_t>((required + 3) / 4));
        std::memcpy(blocks.data(), source, static_cast<size_t>(required));
        std::vector<uint32_t> bgra(static_cast<size_t>(count));
        const auto format = bytes[10] == 0 ? BlitFormat_Dxt1 : bytes[10] == 1 ? BlitFormat_Dxt3 : BlitFormat_Dxt5;
        C2iVector dimensions{static_cast<int32_t>(result.width), static_cast<int32_t>(result.height)};
        need(Blit(dimensions, BlitAlpha_0, blocks.data(), blocksAcross * blockBytes,
            format, bgra.data(), result.width * 4, BlitFormat_Argb8888) != 0);
        for (size_t i = 0; i < count; ++i) {
            const auto color = bgra[i];
            result.rgba[i * 4] = static_cast<uint8_t>(color >> 16);
            result.rgba[i * 4 + 1] = static_cast<uint8_t>(color >> 8);
            result.rgba[i * 4 + 2] = static_cast<uint8_t>(color);
            result.rgba[i * 4 + 3] = static_cast<uint8_t>(color >> 24);
        }
        break;
    }
    case 3: // raw BGRA
        need(size >= count * 4);
        result.rgba.resize(static_cast<size_t>(count) * 4);
        for (size_t i = 0; i < count; ++i) {
            result.rgba[i * 4] = source[i * 4 + 2]; result.rgba[i * 4 + 1] = source[i * 4 + 1];
            result.rgba[i * 4 + 2] = source[i * 4]; result.rgba[i * 4 + 3] = source[i * 4 + 3];
        }
        break;
    default: throw std::runtime_error("Unsupported BLP2 color encoding");
    }
    return result;
}
}
