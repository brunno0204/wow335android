#include "TerrainAlpha.h"
#include "M2File.h"
#include <algorithm>
namespace wow::android {
TerrainAlphaMode decodeTerrainAlphaMode(const std::vector<uint8_t> &b, uint32_t x, uint32_t y) {
    m2::require(x < 64 && y < 64 && b.size() >= 12 && b.size() <= 4 * 1024 * 1024,
                "Invalid terrain WDT input");
    bool version = false, header = false, main = false;
    uint32_t flags = 0;
    bool present = false;
    size_t work = 0;
    for (size_t at = 0; at < b.size();) {
        m2::require(++work <= 4096 && b.size() - at >= 8, "Invalid WDT chunk header/budget");
        const auto tag = m2::u32(b, at), size = m2::u32(b, at + 4);
        at += 8;
        m2::require(size <= b.size() - at, "WDT chunk exceeds file");
        if (tag == 0x4d564552) {
            m2::require(!version && at == 8 && size == 4 && m2::u32(b, at) == 18,
                        "Invalid WDT version");
            version = true;
        } else if (tag == 0x4d504844) {
            m2::require(!header && size == 32, "Invalid WDT map header");
            flags = m2::u32(b, at);
            header = true;
        } else if (tag == 0x4d41494e) {
            m2::require(!main && size == 4096 * 8, "Invalid WDT tile table");
            present = (m2::u32(b, at + 8 * (y * 64 + x)) & 1) != 0;
            main = true;
        }
        at += size;
    }
    m2::require(version && header && main && present && !(flags & 1),
                "WDT has no supported terrain tile");
    return (flags & 4) ? TerrainAlphaMode::Big8 : TerrainAlphaMode::Legacy4;
}
TerrainAlphaMode loadTerrainAlphaMode(const std::string &map, uint32_t x, uint32_t y) {
    terrainTilePath(map, x, y);
    return decodeTerrainAlphaMode(
        m2::readFile("World\\Maps\\" + map + "\\" + map + ".wdt", 4 * 1024 * 1024), x, y);
}
std::vector<uint8_t> decodeTerrainAlpha(const std::vector<uint8_t> &b, size_t offset, size_t end,
                                        TerrainAlphaMode mode, bool compressed,
                                        bool preserveBorder) {
    m2::require(offset <= end && end <= b.size() &&
                    (mode == TerrainAlphaMode::Legacy4 || mode == TerrainAlphaMode::Big8),
                "Invalid terrain alpha input");
    std::vector<uint8_t> out(4096);
    if (mode == TerrainAlphaMode::Legacy4) {
        m2::require(!compressed && end - offset >= 2048, "Invalid legacy terrain alpha size/flags");
        for (size_t i = 0; i < 4096; ++i)
            out[i] = uint8_t(((b[offset + i / 2] >> ((i & 1) * 4)) & 15) * 17);
        if (!preserveBorder)
            for (size_t y = 0; y < 64; ++y)
                for (size_t x = 0; x < 64; ++x)
                    out[y * 64 + x] = out[std::min<size_t>(y, 62) * 64 + std::min<size_t>(x, 62)];
    } else if (!compressed) {
        m2::require(end - offset >= 4096, "Truncated big terrain alpha");
        std::copy_n(b.begin() + offset, 4096, out.begin());
    } else {
        size_t cursor = offset;
        for (size_t row = 0; row < 64; ++row) {
            size_t col = 0;
            while (col < 64) {
                m2::require(cursor < end, "Truncated terrain alpha RLE header");
                const uint8_t control = b[cursor++];
                const size_t count = control & 127;
                m2::require(count && count <= 64 - col, "Invalid terrain alpha RLE row run");
                if (control & 128) {
                    m2::require(cursor < end, "Truncated terrain alpha RLE fill");
                    std::fill_n(out.begin() + row * 64 + col, count, b[cursor++]);
                } else {
                    m2::require(count <= end - cursor, "Truncated terrain alpha RLE literal");
                    std::copy_n(b.begin() + cursor, count, out.begin() + row * 64 + col);
                    cursor += count;
                }
                col += count;
            }
        }
    }
    return out;
}
} // namespace wow::android
