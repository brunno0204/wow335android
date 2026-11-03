// GL thread: outdoor light and sky from the client tables, after WoW/Source/DB/DBClient/Lights.cpp
// (GetBandTimeRange, InterpolateIntBand, TransformMapCoord, InterpFloatBand) and DayNight.
#include "WorldSky.h"
#include "AssetLock.h"
#include "ClientDb.h"
#include "Engine/Source/Gx/CGxVertexPC.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "M2File.h"
#include "WorldLighting.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <vector>
namespace wow::android {
namespace {
struct Band {
    int32_t num = 0;
    std::array<int32_t, 16> time{};
    std::array<uint32_t, 16> data{}; // int band: 0x00RRGGBB; float band: float bits
};
struct Light {
    float x = 0, y = 0, z = 0, start = 0, end = 0;
    uint32_t params = 0;
    bool global = false;
};
struct Tables {
    bool loaded = false, failed = false;
    std::vector<uint8_t> raw;
    std::map<uint32_t, Band> intBands, floatBands;
    struct Row {
        uint32_t map;
        float coords[3], start, end;
        uint32_t params[8];
    };
    std::vector<Row> lights;
} tables;
struct State {
    uint32_t map = UINT32_MAX;
    std::vector<Light> lights;
    uint32_t gameHalfMinutes = 0;
    uint64_t gameAtMs = 0;
    bool gameTime = false;
    uint32_t clear = 0xff78afe6u;
    uint32_t river = 0xff2a5a78u, ocean = 0xff1d4a6au;
} sky;
uint64_t steadyMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
void loadBands(const char *path, std::map<uint32_t, Band> &out) {
    DbcTable table(m2::readFile(path, 16 * 1024 * 1024), 34, 136, path);
    for (uint32_t r = 0; r < table.rows(); ++r) {
        Band band;
        band.num = std::clamp(table.i32(r, 1), 0, 16);
        for (int i = 0; i < 16; ++i) {
            band.time[size_t(i)] = table.i32(r, 2 + uint32_t(i));
            band.data[size_t(i)] = table.u32(r, 18 + uint32_t(i));
        }
        out[table.u32(r, 0)] = band;
    }
}
void loadTables() {
    if (tables.loaded || tables.failed)
        return;
    try {
        std::lock_guard<std::mutex> io(assetIoLock());
        loadBands("DBFilesClient\\LightIntBand.dbc", tables.intBands);
        loadBands("DBFilesClient\\LightFloatBand.dbc", tables.floatBands);
        DbcTable lights(m2::readFile("DBFilesClient\\Light.dbc", 4 * 1024 * 1024), 15, 60, "Light.dbc");
        for (uint32_t r = 0; r < lights.rows(); ++r) {
            Tables::Row row{};
            row.map = lights.u32(r, 1);
            for (int i = 0; i < 3; ++i)
                row.coords[i] = lights.f32(r, 2 + uint32_t(i));
            row.start = lights.f32(r, 5);
            row.end = lights.f32(r, 6);
            for (int i = 0; i < 8; ++i)
                row.params[i] = lights.u32(r, 7 + uint32_t(i));
            tables.lights.push_back(row);
        }
        tables.loaded = true;
    } catch (const std::exception &) {
        tables.failed = true;
    }
}
// GetBandTimeRange (0x007EAE70).
void bandRange(const Band &band, int32_t time, int32_t &index, float &t, int32_t &next) {
    next = 0;
    t = 0;
    index = 0;
    const int32_t count = band.num;
    if (count <= 0)
        return;
    do {
        const int32_t start = band.time[size_t(index)];
        next = (index + 1) % count;
        int32_t end = band.time[size_t(next)];
        if (end > start) {
            if (time >= start && time <= end) {
                t = float(time - start) / float(end - start);
                return;
            }
        } else if (time <= end || time >= start) {
            end += 2880;
            if (time < start)
                time += 2880;
            t = end != start ? float(time - start) / float(end - start) : 0;
            return;
        }
        ++index;
    } while (index < count);
    index = 0;
}
struct Rgb {
    float r = 0, g = 0, b = 0;
};
Rgb mix(const Rgb &a, const Rgb &b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
// InterpolateIntBand (0x007EB070): params band id = band + 18 * paramsId - 17.
Rgb intBand(uint32_t params, uint32_t band, int32_t time) {
    const auto found = tables.intBands.find(band + 18 * params - 17);
    if (found == tables.intBands.end() || !found->second.num)
        return {};
    int32_t index, next;
    float t;
    bandRange(found->second, time, index, t, next);
    auto rgb = [](uint32_t v) { return Rgb{float((v >> 16) & 0xff), float((v >> 8) & 0xff), float(v & 0xff)}; };
    return mix(rgb(found->second.data[size_t(index)]), rgb(found->second.data[size_t(next)]), t);
}
// InterpFloatBand (0x007EAEF0): band 0 is a distance in inches (1/36 yard).
float floatBand(uint32_t params, uint32_t band, int32_t time) {
    const auto found = tables.floatBands.find(band + 6 * params - 5);
    if (found == tables.floatBands.end() || !found->second.num)
        return 0;
    int32_t lo, hi;
    float t;
    bandRange(found->second, time, lo, t, hi);
    float a, b;
    std::memcpy(&a, &found->second.data[size_t(lo)], 4);
    std::memcpy(&b, &found->second.data[size_t(hi)], 4);
    if (band) // bandIndex != 0
        return (b - a) * t + a;
    return a / 36.f + (b / 36.f - a / 36.f) * t;
}
int32_t timeOfDay() {
    if (sky.gameTime) {
        const uint64_t elapsed = (steadyMs() - sky.gameAtMs) / 30000; // half-minutes
        return int32_t((sky.gameHalfMinutes + elapsed) % 2880);
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return (local.tm_hour * 60 + local.tm_min) * 2 + (local.tm_sec >= 30 ? 1 : 0);
}
struct Sample {
    Rgb diffuse, ambient, sky[5], fog, river, ocean;
    float fogEnd = 0, fogScale = 0;
};
Sample sampleParams(uint32_t params, int32_t time) {
    Sample s;
    s.diffuse = intBand(params, 0, time);
    s.ambient = intBand(params, 1, time);
    for (uint32_t i = 0; i < 5; ++i)
        s.sky[i] = intBand(params, 2 + i, time);
    s.fog = intBand(params, 7, time);
    s.river = intBand(params, 13, time); // river shallow
    s.ocean = intBand(params, 15, time); // ocean shallow
    s.fogEnd = floatBand(params, 0, time);
    s.fogScale = floatBand(params, 1, time);
    return s;
}
Sample blend(const Sample &a, const Sample &b, float t) {
    Sample s;
    s.diffuse = mix(a.diffuse, b.diffuse, t);
    s.ambient = mix(a.ambient, b.ambient, t);
    for (int i = 0; i < 5; ++i)
        s.sky[i] = mix(a.sky[i], b.sky[i], t);
    s.fog = mix(a.fog, b.fog, t);
    s.river = mix(a.river, b.river, t);
    s.ocean = mix(a.ocean, b.ocean, t);
    s.fogEnd = a.fogEnd + (b.fogEnd - a.fogEnd) * t;
    s.fogScale = a.fogScale + (b.fogScale - a.fogScale) * t;
    return s;
}
CImVector color(const Rgb &c, uint8_t a = 255) {
    CImVector v;
    v.r = uint8_t(std::clamp(c.r, 0.f, 255.f));
    v.g = uint8_t(std::clamp(c.g, 0.f, 255.f));
    v.b = uint8_t(std::clamp(c.b, 0.f, 255.f));
    v.a = a;
    return v;
}
uint32_t argb(const Rgb &c) {
    const auto v = color(c);
    return 0xff000000u | uint32_t(v.r) << 16 | uint32_t(v.g) << 8 | v.b;
}
} // namespace

void worldSkySetGameTime(uint32_t halfMinutes, uint64_t atMs) {
    sky.gameHalfMinutes = halfMinutes % 2880;
    sky.gameAtMs = atMs;
    sky.gameTime = true;
}
void worldSkyMap(uint32_t map) {
    if (map == sky.map)
        return;
    loadTables();
    sky.map = map;
    sky.lights.clear();
    // TransformMapCoord (0x007EAF60): inches -> yards, game -> world axes, continent offset.
    constexpr float offset = 51200.f / 3.f;
    for (const auto &row : tables.lights) {
        if (row.map != map)
            continue;
        Light light;
        light.params = row.params[0];
        light.global = row.coords[0] == 0 && row.coords[1] == 0 && row.coords[2] == 0;
        if (!light.global) {
            const float gx = row.coords[0] / 36.f, gy = row.coords[1] / 36.f, gz = row.coords[2] / 36.f;
            light.x = -(gz - offset);
            light.y = -(gx - offset);
            light.z = gy;
            light.start = row.start / 36.f;
            light.end = row.end / 36.f;
        }
        sky.lights.push_back(light);
    }
    // Maps without their own global light use the first global light (Eastern Kingdoms).
    const bool hasGlobal = std::any_of(sky.lights.begin(), sky.lights.end(), [](const Light &l) { return l.global; });
    if (!hasGlobal)
        for (const auto &row : tables.lights)
            if (row.coords[0] == 0 && row.coords[1] == 0 && row.coords[2] == 0) {
                Light light;
                light.global = true;
                light.params = row.params[0];
                sky.lights.push_back(light);
                break;
            }
}
uint32_t worldSkyClearColor() { return sky.clear; }
uint32_t worldSkyWaterColor(bool ocean) { return ocean ? sky.ocean : sky.river; }
void worldSkyEnd() { worldLighting().active = false; }
void worldSkyDraw(const C3Vector &eye, const C3Vector &player, const C44Matrix &view, const C44Matrix &projection,
                  float maxFog) {
    auto &lighting = worldLighting();
    lighting.active = false;
    if (!tables.loaded || sky.lights.empty())
        return;
    const int32_t time = timeOfDay();
    const Light *global = nullptr;
    const Light *local = nullptr;
    float localWeight = 0;
    for (const auto &light : sky.lights) {
        if (light.global) {
            if (!global)
                global = &light;
            continue;
        }
        const float d = std::sqrt((player.x - light.x) * (player.x - light.x) + (player.y - light.y) * (player.y - light.y) +
                                  (player.z - light.z) * (player.z - light.z));
        if (d >= light.end)
            continue;
        const float w = d <= light.start ? 1.f : (light.end - d) / std::max(1.f, light.end - light.start);
        if (w > localWeight) {
            localWeight = w;
            local = &light;
        }
    }
    if (!global && !local)
        return;
    Sample s = global ? sampleParams(global->params, time) : sampleParams(local->params, time);
    if (global && local)
        s = blend(s, sampleParams(local->params, time), localWeight);
    // Frame light: fog and the tint of the unlit materials (ambient + sun).
    lighting.active = true;
    float fogEnd = s.fogEnd > 10 ? s.fogEnd : 400.f;
    fogEnd = std::min(fogEnd, maxFog);
    float scale = s.fogScale;
    if (!(scale > 0.f && scale < 1.f))
        scale = .25f;
    lighting.fog = true;
    lighting.fogEnd = fogEnd;
    lighting.fogStart = std::max(5.f, fogEnd * scale);
    lighting.fogColor = argb(s.fog);
    const Rgb tint{std::min(255.f, s.ambient.r + s.diffuse.r * .75f), std::min(255.f, s.ambient.g + s.diffuse.g * .75f),
                   std::min(255.f, s.ambient.b + s.diffuse.b * .75f)};
    // Keep the night readable on a phone screen.
    auto channel = [](float v) { return uint8_t(std::clamp(v * 1.15f + 30.f, 70.f, 255.f)); };
    lighting.r = channel(tint.r);
    lighting.g = channel(tint.g);
    lighting.b = channel(tint.b);
    sky.clear = argb(s.fog);
    if (s.river.r + s.river.g + s.river.b > 1)
        sky.river = argb(s.river);
    if (s.ocean.r + s.ocean.g + s.ocean.b > 1)
        sky.ocean = argb(s.ocean);
    // Sky dome around the eye: zenith .. horizon bands (SkyTop, SkyMiddle, SkyBand1, SkyBand2,
    // SkySmog), fog below the horizon.
    constexpr int segments = 24;
    const float elevations[] = {-.6f, -.08f, 0.f, .09f, .22f, .45f, .9f, 1.5708f};
    const Rgb rings[] = {s.fog, s.fog, s.sky[4], s.sky[3], s.sky[2], s.sky[1], mix(s.sky[1], s.sky[0], .6f), s.sky[0]};
    constexpr int ringCount = int(sizeof(elevations) / sizeof(elevations[0]));
    const float radius = 900;
    std::vector<CGxVertexPC> vertices;
    vertices.reserve(size_t(segments * ringCount));
    for (int ring = 0; ring < ringCount; ++ring) {
        const float e = elevations[ring], ce = std::cos(e), se = std::sin(e);
        for (int i = 0; i < segments; ++i) {
            const float a = float(i) * 6.2831853f / segments;
            CGxVertexPC v;
            v.p = {eye.x + std::cos(a) * ce * radius, eye.y + std::sin(a) * ce * radius, eye.z + se * radius};
            v.c = color(rings[ring]);
            vertices.push_back(v);
        }
    }
    std::vector<uint16_t> indices;
    for (int ring = 0; ring + 1 < ringCount; ++ring)
        for (int i = 0; i < segments; ++i) {
            const uint16_t a = uint16_t(ring * segments + i), b = uint16_t(ring * segments + (i + 1) % segments),
                           c = uint16_t((ring + 1) * segments + i), d = uint16_t((ring + 1) * segments + (i + 1) % segments);
            indices.insert(indices.end(), {a, b, c, b, d, c});
        }
    struct ProjectionMode {
        int32_t previous = GxMasterEnable(GxMasterEnable_NormalProjection);
        ~ProjectionMode() { GxMasterEnableSet(GxMasterEnable_NormalProjection, previous); }
    } projectionMode;
    GxMasterEnableSet(GxMasterEnable_NormalProjection, 1);
    GxXformSetProjection(projection);
    GxXformSetView(view);
    GxXformSet(GxXform_World, C44Matrix(1));
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_Texture0, static_cast<void *>(nullptr));
    GxRsSet(GxRs_Texture1, static_cast<void *>(nullptr));
    GxRsSet(GxRs_TextureShader0, 0);
    GxRsSet(GxRs_TextureShader1, 0);
    GxRsSet(GxRs_TexGen0, GxTexGen_Disabled);
    GxRsSet(GxRs_TexGen1, GxTexGen_Disabled);
    GxRsSet(GxRs_ColorOp0, GxTexOp_PassThru);
    GxRsSet(GxRs_ColorOp1, GxTexOp_PassThru);
    GxRsSet(GxRs_AlphaOp0, GxTexOp_PassThru);
    GxRsSet(GxRs_AlphaOp1, GxTexOp_PassThru);
    GxRsSet(GxRs_MatDiffuse, int32_t(0xffffffffu));
    GxRsSet(GxRs_AlphaRef, 0);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Culling, 0);
    GxPrimVertexPtr(vertices.size(), &vertices[0].p, sizeof(CGxVertexPC), nullptr, 0, &vertices[0].c,
                    sizeof(CGxVertexPC), nullptr, 0, nullptr, 0);
    GxPrimIndexPtr(indices.size(), indices.data());
    CGxBatch batch{GxPrim_Triangles, 0, uint32_t(indices.size()), 0, uint16_t(vertices.size() - 1)};
    GxDraw(&batch, 1);
    GxRsSet(GxRs_DepthTest, 1);
    GxRsSet(GxRs_DepthWrite, 1);
}
} // namespace wow::android
