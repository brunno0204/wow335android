// Minimap for the original interface: the client's minimap tiles (textures\Minimap via
// md5translate.trs, one 256x256 BLP per ADT) composed around the player into a round
// procedural texture that a Texture region on the Minimap frame displays.
#include "UiMinimap.h"
#include "M2File.h"
#include "TextureDecoder.h"
#include "WorldScene.h"
#include "ClientDb.h"
#include "Engine/Source/Frame/CSimpleTexture.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Services/Texture.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace wow::android {
namespace {
constexpr uint32_t kSize = 256;
constexpr double kTile = 533.33333;
// CGMinimapFrame outdoor view radius (yards) per zoom level.
constexpr double kZoomRadius[] = {233.3333, 200.0, 166.6667, 133.3333, 100.0, 66.6667};

using PoseSource = bool (*)(uint32_t &, double &, double &, double &, double &);
PoseSource poseSource = worldScenePlayerPose;
struct Minimap {
    CTexture *texture = nullptr;
    std::vector<uint32_t> pixels = std::vector<uint32_t>(kSize * kSize, 0);
    std::unordered_map<std::string, std::string> translate; // "azeroth\map32_48.blp" -> file
    bool translateLoaded = false;
    std::map<uint32_t, std::string> mapDirectories;
    struct Tile {
        uint64_t key;
        TexturePixels pixels; // empty: no tile
    };
    std::list<Tile> tiles; // most recent first
    int zoom = 0;
    uint32_t map = ~0u;
    double x = 1e9, y = 1e9;
    int drawnZoom = -1;
    uint64_t lastMs = 0;
} mm;

std::string lower(std::string text) {
    for (char &c : text)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
void loadTranslate() {
    if (mm.translateLoaded)
        return;
    mm.translateLoaded = true;
    std::vector<uint8_t> bytes;
    try {
        bytes = m2::readFile("textures\\Minimap\\md5translate.trs", 16 * 1024 * 1024);
    } catch (const std::exception &) {
        return;
    }
    std::string text(bytes.begin(), bytes.end());
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || line.rfind("dir:", 0) == 0)
            continue;
        mm.translate[lower(line.substr(0, tab))] = line.substr(tab + 1);
    }
}
std::string mapDirectory(uint32_t map) {
    if (const auto found = mm.mapDirectories.find(map); found != mm.mapDirectories.end())
        return found->second;
    std::string directory;
    try {
        DbcTable table(m2::readFile("DBFilesClient\\Map.dbc", 16 * 1024 * 1024), 0, 0, "Map");
        for (uint32_t r = 0; r < table.rows(); ++r)
            if (table.u32(r, 0) == map) {
                directory = table.str(r, 1);
                break;
            }
    } catch (const std::exception &) {
    }
    mm.mapDirectories[map] = directory;
    return directory;
}
const TexturePixels &tile(const std::string &directory, int tx, int ty) {
    const uint64_t key = (uint64_t(std::hash<std::string>{}(directory)) << 16) ^ uint64_t(tx * 64 + ty);
    for (auto it = mm.tiles.begin(); it != mm.tiles.end(); ++it)
        if (it->key == key) {
            mm.tiles.splice(mm.tiles.begin(), mm.tiles, it);
            return mm.tiles.front().pixels;
        }
    Minimap::Tile entry{key, {}};
    if (tx >= 0 && ty >= 0 && tx < 64 && ty < 64) {
        char name[96];
        std::snprintf(name, sizeof(name), "\\map%d_%d.blp", tx, ty);
        const auto found = mm.translate.find(lower(directory + name));
        if (found != mm.translate.end())
            try {
                entry.pixels = decodeBlp2(m2::readFile("textures\\Minimap\\" + found->second, 4 * 1024 * 1024));
            } catch (const std::exception &) {
                entry.pixels = {};
            }
    }
    mm.tiles.push_front(std::move(entry));
    if (mm.tiles.size() > 16)
        mm.tiles.pop_back();
    return mm.tiles.front().pixels;
}
// Player-centred composite, north up, round with a soft one-pixel edge.
void compose(const std::string &directory, double px, double py) {
    const double radius = kZoomRadius[std::clamp(mm.zoom, 0, 5)];
    const double center = kSize / 2.0;
    for (uint32_t j = 0; j < kSize; ++j)
        for (uint32_t i = 0; i < kSize; ++i) {
            const double dx = (i + .5 - center) / center, dy = (j + .5 - center) / center;
            const double r = std::sqrt(dx * dx + dy * dy);
            uint32_t &out = mm.pixels[j * kSize + i];
            if (r > 1.0) {
                out = 0;
                continue;
            }
            // Screen right = east (-y), screen down = south (-x).
            const double wx = px - dy * radius, wy = py - dx * radius;
            const double fx = 32.0 - wy / kTile, fy = 32.0 - wx / kTile;
            const int tx = int(std::floor(fx)), ty = int(std::floor(fy));
            const auto &t = tile(directory, tx, ty);
            uint32_t color = 0xff10181c; // no tile: dark
            if (t.width && t.height) {
                const auto u = std::min<uint32_t>(t.width - 1, uint32_t((fx - tx) * t.width));
                const auto v = std::min<uint32_t>(t.height - 1, uint32_t((fy - ty) * t.height));
                const uint8_t *p = &t.rgba[(size_t(v) * t.width + u) * 4];
                color = 0xff000000u | uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
            }
            const double edge = std::clamp((1.0 - r) * center, 0.0, 1.0);
            out = (uint32_t(edge * 255) << 24) | (color & 0xffffff);
        }
}
void textureCallback(EGxTexCommand command, uint32_t, uint32_t, uint32_t, uint32_t, void *, uint32_t &stride,
                     const void *&texels) {
    if (command == GxTex_Latch) {
        stride = kSize * 4;
        texels = mm.pixels.data();
    }
}
CTexture *minimapTexture() {
    if (!mm.texture) {
        CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 0, 1);
        mm.texture = TextureCreate(kSize, kSize, GxTex_Argb8888, GxTex_Argb8888, flags, nullptr, textureCallback,
                                   "AndroidMinimap", 0);
    }
    return mm.texture;
}
// __AndroidMinimapAttach(texture): shows the composed minimap in a Texture region.
int lAttach(lua_State *L) {
    auto *region = static_cast<CSimpleTexture *>(FrameScript_GetObjectThis(L, CSimpleTexture::GetObjectType()));
    if (region && minimapTexture())
        region->SetTexture(minimapTexture());
    return 0;
}
int lZoom(lua_State *L) {
    if (lua_isnumber(L, 1))
        mm.zoom = std::clamp(int(lua_tonumber(L, 1)), 0, 5);
    return 0;
}
// __AndroidPlayerPose() -> map, x, y, z, heading (radians).
int lPose(lua_State *L) {
    uint32_t map;
    double x, y, z, heading;
    if (!poseSource(map, x, y, z, heading))
        return 0;
    lua_pushnumber(L, map);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, z);
    lua_pushnumber(L, heading);
    return 5;
}
} // namespace

void uiMinimapRegister() {
    FrameScript_RegisterFunction("__AndroidMinimapAttach", lAttach);
    FrameScript_RegisterFunction("__AndroidMinimapZoom", lZoom);
    FrameScript_RegisterFunction("__AndroidPlayerPose", lPose);
}
void uiMinimapUpdate(uint64_t nowMs) {
    if (!mm.texture || nowMs - mm.lastMs < 200)
        return;
    mm.lastMs = nowMs;
    uint32_t map;
    double x, y, z, heading;
    if (!poseSource(map, x, y, z, heading))
        return;
    const double moved = std::hypot(x - mm.x, y - mm.y);
    if (map == mm.map && mm.drawnZoom == mm.zoom && moved < kZoomRadius[mm.zoom] / 128.0)
        return;
    loadTranslate();
    const auto directory = mapDirectory(map);
    if (directory.empty())
        return;
    if (map != mm.map)
        mm.tiles.clear();
    mm.map = map;
    mm.x = x;
    mm.y = y;
    mm.drawnZoom = mm.zoom;
    compose(directory, x, y);
    if (CGxTex *gx = TextureGetGxTex(mm.texture, 1, nullptr))
        GxTexUpdate(gx, 0, 0, int32_t(kSize), int32_t(kSize), 1);
}
void uiMinimapReset() { mm = Minimap{}; }
void uiMinimapSetPoseSource(bool (*source)(uint32_t &, double &, double &, double &, double &)) {
    poseSource = source ? source : worldScenePlayerPose;
}
} // namespace wow::android
