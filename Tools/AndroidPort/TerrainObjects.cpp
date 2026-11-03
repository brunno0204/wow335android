#include "TerrainObjects.h"
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
namespace wow::android {
namespace {
constexpr uint32_t tag(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 |
           uint32_t(uint8_t(s[3]));
}
uint32_t u32(const std::vector<uint8_t> &b, size_t o) {
    if (o + 4 > b.size())
        throw std::invalid_argument("Truncated ADT object chunk");
    return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24;
}
uint16_t u16(const std::vector<uint8_t> &b, size_t o) {
    if (o + 2 > b.size())
        throw std::invalid_argument("Truncated ADT object chunk");
    return uint16_t(b[o] | b[o + 1] << 8);
}
float f32(const std::vector<uint8_t> &b, size_t o) {
    const uint32_t bits = u32(b, o);
    float v;
    std::memcpy(&v, &bits, 4);
    return std::isfinite(v) ? v : 0.f;
}
struct Span {
    size_t data = 0, size = 0;
    bool found = false;
};
std::string stringAt(const std::vector<uint8_t> &b, const Span &block, uint32_t offset) {
    if (!block.found || offset >= block.size)
        return {};
    size_t at = block.data + offset, end = at;
    while (end < block.data + block.size && b[end])
        ++end;
    return std::string(b.begin() + long(at), b.begin() + long(end));
}
std::string asM2(std::string path) {
    const auto dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = path.substr(dot);
        for (char &c : ext)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".mdx" || ext == ".mdl")
            path = path.substr(0, dot) + ".m2";
    }
    return path;
}
} // namespace
TerrainObjects decodeTerrainObjects(const std::vector<uint8_t> &b) {
    std::map<uint32_t, Span> chunks;
    for (size_t at = 0; at + 8 <= b.size();) {
        const uint32_t id = u32(b, at), size = u32(b, at + 4);
        if (size > b.size() - at - 8)
            break;
        auto &span = chunks[id];
        if (!span.found)
            span = {at + 8, size, true};
        at += 8 + size;
    }
    const float center = 32.f * (1600.f / 3.f);
    auto place = [&](const Span &entries, size_t stride, const Span &ids, const Span &names,
                     bool doodad) {
        std::vector<TerrainPlacementDef> out;
        if (!entries.found || !ids.found || !names.found)
            return out;
        const size_t count = entries.size / stride, idCount = ids.size / 4;
        for (size_t i = 0; i < count && i < 65536; ++i) {
            const size_t e = entries.data + i * stride;
            const uint32_t nameId = u32(b, e);
            if (nameId >= idCount)
                continue;
            TerrainPlacementDef def;
            def.path = stringAt(b, names, u32(b, ids.data + nameId * 4));
            if (def.path.empty())
                continue;
            if (doodad)
                def.path = asM2(def.path);
            def.uniqueId = u32(b, e + 4);
            const float px = f32(b, e + 8), py = f32(b, e + 12), pz = f32(b, e + 16);
            def.position = {center - pz, center - px, py};
            const float rx = f32(b, e + 20), ry = f32(b, e + 24), rz = f32(b, e + 28);
            constexpr float rad = 0.017453292f;
            def.rotation = {rz * rad, rx * rad, ry * rad};
            if (doodad) {
                def.scale = float(u16(b, e + 32)) * 0.0009765625f;
                def.flags = u16(b, e + 34);
            } else {
                def.flags = u16(b, e + 56);
                def.doodadSet = u16(b, e + 58);
            }
            out.push_back(std::move(def));
        }
        return out;
    };
    TerrainObjects objects;
    objects.doodads = place(chunks[tag("MDDF")], 36, chunks[tag("MMID")], chunks[tag("MMDX")], true);
    objects.mapObjects = place(chunks[tag("MODF")], 64, chunks[tag("MWID")], chunks[tag("MWMO")], false);
    return objects;
}
} // namespace wow::android
