#include "WorldAssets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
namespace wow::android {
namespace {
uint32_t u32(const std::vector<uint8_t> &b, size_t o) {
    if (o > b.size() || b.size() - o < 4)
        throw std::invalid_argument("Truncated client data record");
    return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 |
           uint32_t(b[o + 3]) << 24;
}
} // namespace
std::string mapDirectory(const std::vector<uint8_t> &b, uint32_t id) {
    if (b.size() < 20 || u32(b, 0) != 0x43424457) // "WDBC"
        throw std::invalid_argument("Invalid Map.dbc header");
    const uint32_t records = u32(b, 4), fields = u32(b, 8), size = u32(b, 12),
                   strings = u32(b, 16);
    if (fields < 2 || size < 8 || size / 4 < fields || records > 65536 ||
        uint64_t(records) * size + strings + 20 != b.size())
        throw std::invalid_argument("Invalid Map.dbc layout");
    const size_t table = 20 + size_t(records) * size;
    for (uint32_t i = 0; i < records; ++i) {
        const size_t row = 20 + size_t(i) * size;
        if (u32(b, row) != id)
            continue;
        const uint32_t offset = u32(b, row + 4);
        if (offset >= strings)
            throw std::invalid_argument("Invalid Map.dbc directory string");
        const auto begin = b.begin() + long(table + offset);
        const auto end = std::find(begin, b.end(), uint8_t(0));
        if (end == b.end())
            throw std::invalid_argument("Unterminated Map.dbc directory string");
        std::string name(begin, end);
        if (name.empty())
            throw std::invalid_argument("Empty map directory");
        return name;
    }
    throw std::out_of_range("Map ID not present in Map.dbc");
}
std::pair<uint32_t, uint32_t> terrainTileFor(double x, double y) {
    constexpr double tile = 1600.0 / 3.0, half = 32 * tile;
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) >= half || std::abs(y) >= half)
        throw std::invalid_argument("Position outside the world grid");
    return {uint32_t(std::floor(32 - y / tile)), uint32_t(std::floor(32 - x / tile))};
}
std::optional<CharacterModelPaths> characterModelPaths(uint8_t race, uint8_t sex, uint8_t skin,
                                                       uint8_t hairColor) {
    static const char *const names[] = {nullptr,  "Human",  "Orc",   "Dwarf",    "NightElf",
                                        "Scourge", "Tauren", "Gnome", "Troll",    nullptr,
                                        "BloodElf", "Draenei"};
    if (race >= std::size(names) || !names[race] || sex > 1)
        return std::nullopt;
    const std::string r = names[race], s = sex ? "Female" : "Male";
    const std::string folder = "Character\\" + r + "\\" + s + "\\";
    char body[32], hair[32];
    std::snprintf(body, sizeof(body), "Skin00_%02u.blp", unsigned(skin));
    std::snprintf(hair, sizeof(hair), "Hair00_%02u.blp", unsigned(hairColor));
    return CharacterModelPaths{folder + r + s + ".m2", folder + r + s + "00.skin",
                               folder + r + s + body, "Character\\" + r + "\\" + hair};
}
std::string characterBaseGeosets() { return "0,1,101,201,301,401,501,701,1301"; }
std::optional<uint32_t> m2SequenceIndex(const std::vector<uint8_t> &b, uint16_t animationId) {
    if (b.size() < 0x24 || u32(b, 0) != 0x3032444d)
        throw std::invalid_argument("Invalid M2 header");
    const uint32_t count = u32(b, 0x1c), offset = u32(b, 0x20);
    if (count > 4096 || offset > b.size() || uint64_t(count) * 64 > b.size() - offset)
        throw std::invalid_argument("Invalid M2 sequence table");
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = offset + size_t(i) * 64;
        if (uint16_t(b[at] | b[at + 1] << 8) == animationId)
            return i;
    }
    return std::nullopt;
}
} // namespace wow::android
