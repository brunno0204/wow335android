// GL-thread in-world scene: adopts worker packs, simulates the local player, draws
// streamed terrain, WMOs, doodads, units and publishes the mover sample/HUD overlay.
#include "WorldScene.h"
#include "Engine/Source/Gx/Gx.h"
#include "WorldLighting.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "Engine/Source/Gx/CGxVertexPC.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "M2PreviewTextures.h"
#include "GxStaticMesh.h"
#include "TerrainPreviewTextures.h"
#include "WorldAssets.h"
#include "WorldLink.h"
#include "WorldPlayer.h"
#include "WorldSky.h"
#include "Frustum.h"
#include "UiGameData.h"
#include <algorithm>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
namespace wow::android {
namespace {
uint64_t steadyMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
struct WaterVertex {
    C3Vector p;
    uint8_t kind; // 0 water, 1 ocean, 2 magma, 3 slime
};
struct TileRender {
    std::unique_ptr<TerrainPreview> terrain;
    std::unique_ptr<TerrainPreviewTextures> textures;
    std::unique_ptr<GxStaticMesh> mesh = std::make_unique<GxStaticMesh>();
    // MH2O liquid surfaces (drawn translucent after the opaque world).
    std::vector<WaterVertex> water;
    std::vector<uint16_t> waterIndices;
    std::unique_ptr<GxStaticMesh> waterMesh; // colored vertices, refreshed a few times per second
    uint64_t waterColoredAt = 0;
};
constexpr float kLiquidCell = 33.333332f / 8;
uint8_t liquidKind(uint16_t type) {
    if (type == 2 || type == 14)
        return 1;
    if (type == 3 || type == 19)
        return 2;
    if (type == 4 || type == 20)
        return 3;
    return 0;
}
void buildWater(TileRender &render) {
    for (const auto &liquid : render.terrain->geometry().liquids) {
        const uint32_t w = liquid.width, h = liquid.height;
        if (render.water.size() + (w + 1) * (h + 1) > 65000)
            break;
        const auto base = uint16_t(render.water.size());
        for (uint32_t j = 0; j <= h; ++j)
            for (uint32_t i = 0; i <= w; ++i)
                render.water.push_back({{liquid.top - float(liquid.yOffset + j) * kLiquidCell,
                                         liquid.left - float(liquid.xOffset + i) * kLiquidCell,
                                         liquid.heights[j * (w + 1) + i]},
                                        liquidKind(liquid.type)});
        for (uint32_t j = 0; j < h; ++j)
            for (uint32_t i = 0; i < w; ++i) {
                if (!liquid.exists[j * w + i])
                    continue;
                const auto a = uint16_t(base + j * (w + 1) + i), b = uint16_t(a + 1), c = uint16_t(a + w + 1),
                           d = uint16_t(c + 1);
                render.waterIndices.insert(render.waterIndices.end(), {a, c, b, b, c, d});
            }
    }
}
struct DoodadModel {
    std::unique_ptr<M2ModelPreview> model;
    std::unique_ptr<M2PreviewTextures> textures;
    std::unique_ptr<GxStaticMesh> mesh = std::make_unique<GxStaticMesh>();
    std::vector<C44Matrix> visible; // per-frame scratch
    // Particle emitters of nearby instances, by rounded position.
    struct Emitters {
        std::unique_ptr<M2ParticleRuntime> runtime;
        uint64_t seenMs = 0;
    };
    std::unordered_map<uint64_t, Emitters> particles;
};
struct MapObjectRender {
    std::unique_ptr<WmoModel> model;
    C44Matrix world{1};
    std::unique_ptr<M2PreviewTextures> textures;
    std::vector<std::unique_ptr<GxStaticMesh>> meshes;
    float cx = 0, cy = 0, cz = 0, radius = 0;
};
struct PackRender {
    uint32_t tile = 0;
    uint64_t mesh = 0;
    std::vector<std::pair<std::string, std::vector<C44Matrix>>> doodads;
    std::vector<MapObjectRender> mapObjects;
};
// One pose per animation state, sharing geometry/materials; item models attach to it.
struct UnitModel {
    std::unique_ptr<DressedModel> dressed; // null: failed
    std::unique_ptr<M2PreviewTextures> textures;
    std::vector<std::unique_ptr<M2PreviewTextures>> itemTextures;
    std::map<uint16_t, std::unique_ptr<M2ModelPreview>> poses; // animation ID -> pose
    std::map<const M2ModelPreview *, std::unique_ptr<GxStaticMesh>> meshes; // per pose / item
    GxStaticMesh &mesh(const M2ModelPreview *pose) {
        auto &slot = meshes[pose];
        if (!slot)
            slot = std::make_unique<GxStaticMesh>();
        return *slot;
    }
    // Per-unit one-shot/timed clips (emotes, swings, casts) with their own clock.
    struct Action {
        uint16_t anim = 0;
        std::unique_ptr<M2ModelPreview> pose;
    };
    std::unordered_map<uint64_t, Action> actions;
    M2ModelPreview *action(uint64_t guid, uint16_t anim, bool loop, bool restart) {
        if (!dressed)
            return nullptr;
        const auto slot = dressed->animations.find(anim);
        if (slot == dressed->animations.end())
            return nullptr;
        auto &a = actions[guid];
        if (!a.pose || a.anim != anim || restart) {
            if (a.pose)
                meshes.erase(a.pose.get());
            a.pose = std::make_unique<M2ModelPreview>(*dressed->body);
            a.anim = anim;
            try {
                a.pose->play(slot->second, loop);
            } catch (const std::exception &) {
            }
        }
        return a.pose.get();
    }
    void dropAction(uint64_t guid) {
        auto found = actions.find(guid);
        if (found == actions.end())
            return;
        meshes.erase(found->second.pose.get());
        actions.erase(found);
    }
    M2ModelPreview *pose(uint16_t id) {
        if (!dressed)
            return nullptr;
        if (auto found = poses.find(id); found != poses.end())
            return found->second.get();
        const auto slot = dressed->animations.find(id);
        if (slot == dressed->animations.end())
            return id ? pose(0) : nullptr;
        auto copy = std::make_unique<M2ModelPreview>(*dressed->body);
        try {
            copy->play(slot->second, true);
        } catch (const std::exception &) {
        }
        return (poses[id] = std::move(copy)).get();
    }
};
struct Scene {
    bool active = false;
    uint32_t epoch = 0, map = 0;
    std::optional<PlacementPack> pendingPlace;
    uint64_t pendingSince = 0;
    WorldPlayer player;
    WorldCollision collision;
    std::map<uint32_t, TileRender> tiles;
    std::vector<PackRender> packs;
    std::unordered_map<std::string, DoodadModel> doodads;
    uint64_t nextMesh = 1;
    std::pair<uint32_t, uint32_t> center{UINT32_MAX, UINT32_MAX};
    // Local player model.
    std::unique_ptr<UnitModel> hero;
    uint16_t heroAnimation = 0xffff;
    bool wasGrounded = true;
    // Units.
    std::unordered_map<std::string, std::unique_ptr<UnitModel>> unitModels;
    std::set<std::string> requested;
    std::vector<UnitView> units;
    uint32_t unitsSerial = 0;
    uint64_t target = 0;
    uint64_t self = 0;
    uint32_t gameTimeSerial = 0;
    uint32_t heroMount = 0;
    uint32_t heroFaction = 0; // player's UNIT_FIELD_FACTIONTEMPLATE (name colors)
    // Active unit animations by GUID: clip, end time, looping state clip, start serial.
    struct ActiveAction {
        uint16_t anim = 0;
        uint64_t until = 0;
        bool loop = false;
        uint32_t serial = 0;
    };
    std::unordered_map<uint64_t, ActiveAction> actions;
    uint32_t actionSerial = 0;
    uint16_t heroAction = 0xffff;
    uint32_t heroActionSerial = 0;
    C44Matrix view{1}, projection{1};
    float aspect = 1;
    CShaderEffect *material = nullptr; // last world material (UI model frames draw with it)
    uint64_t now = 0;
};
Scene scene;
std::atomic<float> pendingZoom{1};
std::mutex overlayMutex;
std::string overlayText; // guarded by overlayMutex
std::unique_ptr<UnitModel> adoptModel(std::unique_ptr<DressedModel> dressed) {
    auto unit = std::make_unique<UnitModel>();
    if (!dressed || !dressed->body)
        return unit;
    unit->textures = std::make_unique<M2PreviewTextures>(dressed->body->model().previewMaterials());
    for (const auto &item : dressed->items)
        unit->itemTextures.push_back(std::make_unique<M2PreviewTextures>(item.model->model().previewMaterials()));
    unit->dressed = std::move(dressed);
    return unit;
}
void clearMap() {
    scene.tiles.clear();
    scene.packs.clear();
    scene.doodads.clear();
    scene.collision.clear();
    scene.center = {UINT32_MAX, UINT32_MAX};
}
void evict(uint32_t key) {
    scene.tiles.erase(key);
    scene.collision.removeTerrain(key);
    for (auto it = scene.packs.begin(); it != scene.packs.end();) {
        if (it->tile == key) {
            scene.collision.removeMesh(it->mesh);
            it = scene.packs.erase(it);
        } else
            ++it;
    }
}
int tileDistance(uint32_t key, std::pair<uint32_t, uint32_t> center) {
    const int first = int(key / 64), second = int(key % 64);
    return std::max(std::abs(first - int(center.first)), std::abs(second - int(center.second)));
}
// Highest ground near a unit's server height (keeps NPCs on WMO floors and terrain),
// cached per unit until it moves.
struct GroundCache {
    float x = 1e30f, y = 1e30f, z = 1e30f, result = 0;
    uint64_t mesh = 0;
};
std::unordered_map<uint64_t, GroundCache> groundCache;
std::unordered_map<uint64_t, uint32_t> actionSerials; // unit GUID -> action serial drawn
std::unordered_map<uint64_t, bool> seenAlive;          // unit GUID -> seen alive (death plays)
float groundZ(uint64_t guid, float x, float y, float z) {
    auto &c = groundCache[guid];
    if (std::abs(c.x - x) < .3f && std::abs(c.y - y) < .3f && std::abs(c.z - z) < .3f && c.mesh == scene.nextMesh)
        return c.result;
    c = {x, y, z, z, scene.nextMesh};
    if (auto ground = scene.collision.groundBelow(x, y, z + 1.5); ground && ground->z > z - 4)
        c.result = float(ground->z);
    return c.result;
}
C44Matrix unitMatrix(float x, float y, float z, float facing, float scale) {
    C44Matrix m(1);
    const float c = std::cos(facing) * scale, s = std::sin(facing) * scale;
    m.a0 = c;
    m.a1 = s;
    m.b0 = -s;
    m.b1 = c;
    m.c2 = scale;
    m.d0 = x;
    m.d1 = y;
    m.d2 = z;
    return m;
}
struct Placed {
    const UnitView *unit;
    float x, y, z, facing;
    bool moving;
};
Placed placeUnit(const UnitView &u, uint64_t now) {
    Placed p{&u, u.x, u.y, u.z, u.facing, false};
    if (u.path.size() >= 2 && u.pathDurationMs) {
        // Linear along the path by distance.
        double total = 0;
        std::vector<double> lengths;
        for (size_t i = 1; i < u.path.size(); ++i) {
            const auto &a = u.path[i - 1], &b = u.path[i];
            lengths.push_back(std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]));
            total += lengths.back();
        }
        const double k = std::clamp(double(now - std::min(now, u.pathStartMs)) / u.pathDurationMs, 0.0, 1.0);
        double along = k * total;
        size_t i = 0;
        while (i + 1 < lengths.size() && along > lengths[i]) {
            along -= lengths[i];
            ++i;
        }
        const auto &a = u.path[i], &b = u.path[i + 1];
        const double t = lengths[i] > 1e-6 ? std::clamp(along / lengths[i], 0.0, 1.0) : 1.0;
        p.x = float(a[0] + (b[0] - a[0]) * t);
        p.y = float(a[1] + (b[1] - a[1]) * t);
        p.z = float(a[2] + (b[2] - a[2]) * t);
        if (std::hypot(b[0] - a[0], b[1] - a[1]) > 1e-3)
            p.facing = float(std::atan2(b[1] - a[1], b[0] - a[0]));
        p.moving = k < 1;
    } else if (u.moving && (u.vx != 0 || u.vy != 0)) {
        const double seconds = std::min(double(now - std::min(now, u.timeMs)) * .001, 1.5);
        p.x = float(u.x + u.vx * seconds);
        p.y = float(u.y + u.vy * seconds);
        p.moving = true;
    }
    if (u.type != ObjectType::GameObject)
        p.z = groundZ(u.guid, p.x, p.y, p.z);
    return p;
}
bool project(float x, float y, float z, float &sx, float &sy) {
    const C44Matrix m = scene.view * scene.projection;
    const float cx = x * m.a0 + y * m.b0 + z * m.c0 + m.d0;
    const float cy = x * m.a1 + y * m.b1 + z * m.c1 + m.d1;
    const float cw = x * m.a3 + y * m.b3 + z * m.c3 + m.d3;
    if (cw <= .1f)
        return false;
    sx = (cx / cw + 1) * .5f;
    sy = (1 - cy / cw) * .5f;
    return sx > -.1f && sx < 1.1f && sy > -.1f && sy < 1.1f;
}
// CGObject_C::s_questStatusIcons (trivial quests 2..4 only with "track low level quests").
uint32_t questIcon(uint8_t status) {
    static const uint32_t icons[11]{0, 6, 1, 2, 3, 5, 10, 11, 4, 9, 9};
    if (status >= 11 || (status >= 2 && status <= 4))
        return 0;
    return icons[status];
}
// CGGameUI::GetSelectionHighlightColor for unit names (RGB).
uint32_t nameColor(const UnitView &u, uint32_t heroFaction) {
    const bool player = u.type == ObjectType::Player;
    if (!player && (u.dead || (u.maxHealth && !u.health)))
        return 0x7f7f7f;
    int reaction = uiFactionReaction(u.factionTemplate, heroFaction);
    if (reaction < 0)
        reaction = player ? 4 : (u.npcFlags ? 4 : 3);
    static const uint32_t colors[8]{0xff0000, 0xff0000, 0xff8000, 0xffff00, 0x00ff00, 0x00ff00, 0x00ff00, 0x00ff00};
    if (player) {
        const bool pvp = (u.flags & 0x1000) != 0; // UNIT_FLAG_PVP
        if (!pvp)
            return 0x6060ff; // s_playerColors[1]
        return colors[std::clamp(reaction, 0, 7)];
    }
    return colors[std::clamp(reaction, 0, 7)];
}
float unitHeight(const UnitModel *model, const UnitView &u) {
    if (model && model->dressed && model->dressed->body) {
        const auto *c = model->dressed->body->boundsCenter();
        const double h = c[2] + model->dressed->body->boundsRadius() * .8;
        return float(std::clamp(h, .5, 30.0)) * model->dressed->scale * u.scale;
    }
    return 2;
}
// Clip length of an animation of a model (ms), or a default.
uint32_t clipLength(const DressedModel *dressed, uint16_t anim) {
    if (!dressed || !dressed->body)
        return 1200;
    const auto slot = dressed->animations.find(anim);
    if (slot == dressed->animations.end())
        return 1200;
    for (const auto &sequence : dressed->body->model().sequences())
        if (sequence.index == slot->second)
            return std::clamp<uint32_t>(sequence.duration, 200, 8000);
    return 1200;
}
// First animation of a fallback chain the model has (the original falls back through
// AnimationData.Fallback; these are the chains for the clips the server drives).
uint16_t availableAnim(const DressedModel *dressed, uint16_t anim) {
    if (!dressed)
        return 0xffff;
    auto has = [&](uint16_t id) { return dressed->animations.count(id) != 0; };
    const std::vector<uint16_t> chain = anim == 18 ? std::vector<uint16_t>{18, 17, 16}
                                      : anim == 17 ? std::vector<uint16_t>{17, 16, 18}
                                      : anim == 125 ? std::vector<uint16_t>{125, 52, 51}
                                      : anim == 54 ? std::vector<uint16_t>{54, 32, 53}
                                      : anim == 52 ? std::vector<uint16_t>{52, 51, 26}
                                      : anim == 21 ? std::vector<uint16_t>{21, 20}
                                      : anim == 13 ? std::vector<uint16_t>{13, 4}
                                      : std::vector<uint16_t>{anim};
    for (uint16_t id : chain)
        if (has(id))
            return id;
    return 0xffff;
}
const DressedModel *modelOf(uint64_t guid) {
    if (guid == scene.self)
        return scene.hero ? scene.hero->dressed.get() : nullptr;
    for (const auto &u : scene.units)
        if (u.guid == guid) {
            const auto found = scene.unitModels.find(u.modelKey);
            return found == scene.unitModels.end() ? nullptr : found->second->dressed.get();
        }
    return nullptr;
}
void queueAction(const UnitAction &a, uint64_t now) {
    if (!a.guid)
        return;
    if (a.anim == 0) { // back to the base animation
        scene.actions.erase(a.guid);
        return;
    }
    const auto *dressed = modelOf(a.guid);
    uint16_t wanted = a.anim;
    // Melee swing by the weapon in hand (CGUnit_C): two-handed 18, one-handed 17, none 16.
    if (wanted == 17 && dressed && dressed->mainHandType == 17)
        wanted = 18;
    const uint16_t anim = availableAnim(dressed, wanted);
    if (anim == 0xffff)
        return;
    auto &slot = scene.actions[a.guid];
    // Being hit (wound, dodge, parry) never cuts the unit's own swing, cast or emote short.
    if (slot.until > now && (a.anim == 9 || a.anim == 30 || a.anim == 21))
        return;
    slot.anim = anim;
    slot.loop = a.loop;
    slot.until = now + (a.ms ? a.ms : clipLength(dressed, anim));
    slot.serial = ++scene.actionSerial;
}
// Current action of a unit, or nullptr (expired, or a looping state cut by moving).
Scene::ActiveAction *activeAction(uint64_t guid, uint64_t now, bool moving) {
    const auto found = scene.actions.find(guid);
    if (found == scene.actions.end())
        return nullptr;
    if (now >= found->second.until ||
        (moving && found->second.anim != 18 && found->second.anim != 17 && found->second.anim != 16)) {
        scene.actions.erase(found);
        return nullptr;
    }
    return &found->second;
}
// Highest liquid surface at (x, y) of the streamed tiles.
std::optional<double> liquidAt(double x, double y) {
    std::pair<uint32_t, uint32_t> tile;
    try {
        tile = terrainTileFor(x, y);
    } catch (const std::exception &) {
        return std::nullopt;
    }
    const auto found = scene.tiles.find(tile.first * 64 + tile.second);
    if (found == scene.tiles.end() || !found->second.terrain)
        return std::nullopt;
    std::optional<double> best;
    for (const auto &liquid : found->second.terrain->geometry().liquids) {
        const double row = (liquid.top - x) / kLiquidCell - liquid.yOffset, col = (liquid.left - y) / kLiquidCell - liquid.xOffset;
        if (row < 0 || col < 0 || row >= liquid.height || col >= liquid.width)
            continue;
        const auto r = uint32_t(row), c = uint32_t(col);
        if (!liquid.exists[r * liquid.width + c])
            continue;
        const double fr = row - r, fc = col - c;
        const uint32_t w = liquid.width + 1u;
        const double h = (liquid.heights[r * w + c] * (1 - fc) + liquid.heights[r * w + c + 1] * fc) * (1 - fr) +
                         (liquid.heights[(r + 1) * w + c] * (1 - fc) + liquid.heights[(r + 1) * w + c + 1] * fc) * fr;
        if (!best || h > *best)
            best = h;
    }
    return best;
}
void drawWater(const double *focus, double range) {
    const uint32_t river = worldSkyWaterColor(false), ocean = worldSkyWaterColor(true);
    auto colour = [&](uint8_t kind, uint64_t now) {
        CImVector c;
        uint32_t v = kind == 1 ? ocean : kind == 2 ? 0xffff6e14u : kind == 3 ? 0xff50c83cu : river;
        v = worldLit(v);
        c.r = uint8_t(v >> 16);
        c.g = uint8_t(v >> 8);
        c.b = uint8_t(v);
        // A slow shimmer instead of the animated water textures.
        const float wave = .5f + .5f * std::sin(float(now % 6283) * .001f);
        c.a = uint8_t(kind == 2 ? 235 : kind == 3 ? 200 : 150 + int(wave * 25));
        return c;
    };
    std::vector<CGxVertexPC> vertices;
    struct ProjectionMode {
        int32_t previous = GxMasterEnable(GxMasterEnable_NormalProjection);
        ~ProjectionMode() { GxMasterEnableSet(GxMasterEnable_NormalProjection, previous); }
    } projectionMode;
    GxMasterEnableSet(GxMasterEnable_NormalProjection, 1);
    GxXformSetProjection(scene.projection);
    GxXformSetView(scene.view);
    GxXformSet(GxXform_World, C44Matrix(1));
    GxRsSet(GxRs_Lighting, 0);
    applyWorldFog();
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
    GxRsSet(GxRs_BlendingMode, GxBlend_Alpha);
    GxRsSet(GxRs_DepthTest, 1);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Culling, 0);
    const uint64_t now = steadyMs();
    const Frustum frustum(scene.view * scene.projection);
    for (auto &[key, tile] : scene.tiles) {
        if (tile.waterIndices.empty())
            continue;
        const auto &first = tile.water.front().p;
        if (std::hypot(first.x - focus[0], first.y - focus[1]) > range + 760)
            continue;
        // Tile-sized sphere around the tile center (key = first * 64 + second).
        const float tileSize = 1600.f / 3.f;
        const float cx = (32.f - float(key % 64)) * tileSize - tileSize / 2, cy = (32.f - float(key / 64)) * tileSize - tileSize / 2;
        if (!frustum.sphere(cx, cy, first.z, tileSize * .75f))
            continue;
        if (!tile.waterMesh || now - tile.waterColoredAt > 120) {
            vertices.resize(tile.water.size());
            CImVector palette[4];
            for (uint8_t kind = 0; kind < 4; ++kind)
                palette[kind] = colour(kind, now);
            for (size_t i = 0; i < tile.water.size(); ++i) {
                vertices[i].p = tile.water[i].p;
                vertices[i].c = palette[std::min<uint8_t>(tile.water[i].kind, 3)];
            }
            if (!tile.waterMesh)
                tile.waterMesh = std::make_unique<GxStaticMesh>();
            tile.waterMesh->setVertices(vertices.data(), uint32_t(vertices.size()), sizeof(CGxVertexPC), GxVBF_PC);
            if (!tile.waterMesh->ready())
                tile.waterMesh->setIndices(tile.waterIndices.data(), uint32_t(tile.waterIndices.size()));
            tile.waterColoredAt = now;
        }
        tile.waterMesh->bind();
        CGxBatch batch{GxPrim_Triangles, 0, uint32_t(tile.waterIndices.size()), 0, uint16_t(tile.water.size() - 1)};
        GxDraw(&batch, 1);
    }
    GxRsSet(GxRs_DepthWrite, 1);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
}
std::string mountKey(uint32_t display) { return "d:" + std::to_string(display); }
UnitModel *mountModel(uint32_t display) {
    if (!display)
        return nullptr;
    const auto found = scene.unitModels.find(mountKey(display));
    return found == scene.unitModels.end() || !found->second->dressed ? nullptr : found->second.get();
}
// Rider placement on the mount's attachment 0 (CGUnit_C attaches the rider model there),
// without the mount's scale.
std::optional<C44Matrix> riderMatrix(UnitModel &mount, uint16_t anim, const C44Matrix &mountWorld, float riderScale) {
    auto *pose = mount.pose(anim);
    if (!pose)
        return std::nullopt;
    auto attached = pose->model().attachment(0, mountWorld);
    if (!attached)
        return std::nullopt;
    auto &m = *attached;
    auto normalize = [&](float &x, float &y, float &z) {
        const float n = std::sqrt(x * x + y * y + z * z);
        if (n > 1e-6f) {
            x = x / n * riderScale;
            y = y / n * riderScale;
            z = z / n * riderScale;
        }
    };
    normalize(m.a0, m.a1, m.a2);
    normalize(m.b0, m.b1, m.b2);
    normalize(m.c0, m.c1, m.c2);
    return m;
}
void publishSample() {
    MoverSample sample;
    const auto &p = scene.player;
    const double mx = p.moveX(), my = p.moveY(), magnitude = std::hypot(mx, my);
    const bool moving = magnitude > 1e-6 && !p.blocked() && p.placed();
    sample.valid = p.placed();
    sample.x = p.x();
    sample.y = p.y();
    sample.z = p.z();
    if (moving) {
        sample.directionX = mx / magnitude;
        sample.directionY = my / magnitude;
        sample.horizontalSpeed = p.speed();
        sample.backward = p.backward();
    }
    sample.walking = p.walking();
    sample.facing = p.heading();
    sample.hasFacing = true;
    sample.grounded = p.onGround() || p.swimming();
    sample.swimming = p.swimming();
    sample.verticalSpeed = p.verticalSpeed();
    std::lock_guard<std::mutex> link(worldLinkMutex());
    auto &state = worldLink();
    if (!state.active)
        return;
    state.sample = sample;
    state.sampleSerial = scene.epoch;
    if (p.placed())
        if (const uint32_t area = scene.collision.areaAt(p.x(), p.y()))
            state.areaId = area;
    scene.player.setRunSpeed(state.runSpeed);
    // Stream the 3x3 ADT neighbourhood around the player.
    if (p.placed()) {
        try {
            const auto tile = terrainTileFor(p.x(), p.y());
            if (tile != scene.center) {
                scene.center = tile;
                std::vector<uint32_t> drop;
                for (const auto &[key, t] : scene.tiles)
                    if (tileDistance(key, tile) > 2)
                        drop.push_back(key);
                for (const auto &pack : scene.packs)
                    if (pack.tile < 64 * 64 && tileDistance(pack.tile, tile) > 2) // 4096: global WMO
                        drop.push_back(pack.tile);
                std::sort(drop.begin(), drop.end());
                drop.erase(std::unique(drop.begin(), drop.end()), drop.end());
                for (uint32_t key : drop) {
                    evict(key);
                    state.evictedTiles.insert(key);
                }
                if (state.loadedSerial >= scene.epoch) {
                    state.request = {++state.nextSerial, scene.map, float(p.x()), float(p.y()), float(p.z()),
                                     float(p.heading()), true};
                }
            }
        } catch (const std::exception &) {
        }
    }
    scene.self = state.guid;
    if (state.gameTimeSerial != scene.gameTimeSerial) {
        scene.gameTimeSerial = state.gameTimeSerial;
        worldSkySetGameTime(state.gameHalfMinutes, steadyMs());
    }
    if (!state.unitActions.empty()) {
        const uint64_t now = steadyMs();
        for (const auto &a : state.unitActions)
            queueAction(a, now);
        state.unitActions.clear();
    }
    if (state.unitsSerial != scene.unitsSerial) {
        scene.units = state.units;
        scene.unitsSerial = state.unitsSerial;
    }
    for (const auto &u : scene.units)
        if (!u.modelKey.empty() && !scene.unitModels.count(u.modelKey) && scene.requested.insert(u.modelKey).second)
            state.modelRequests.push_back(u.modelKey);
    // Mount models (CreatureDisplayInfo) of mounted units and of the player.
    scene.heroMount = state.hud.mountDisplay;
    scene.heroFaction = state.hud.factionTemplate;
    // Quest giver marks: the interact icon models (CGObject_C::s_interactIcons).
    for (const auto &u : scene.units)
        if (const uint32_t icon = questIcon(u.questStatus)) {
            const auto key = "i:" + std::to_string(icon);
            if (!scene.unitModels.count(key) && scene.requested.insert(key).second)
                state.modelRequests.push_back(key);
        }
    auto wantMount = [&](uint32_t display) {
        if (!display)
            return;
        const auto key = mountKey(display);
        if (!scene.unitModels.count(key) && scene.requested.insert(key).second)
            state.modelRequests.push_back(key);
    };
    wantMount(scene.heroMount);
    for (const auto &u : scene.units)
        wantMount(u.mountDisplay);
}
uint64_t heroLastSwingMs = 0;
// The client draws its weapons itself when melee starts (CGUnit_C sheathe state); the server's
// UNIT_FIELD_BYTES_2 sheath only follows CMSG_SETSHEATHED, so combat and recent swings count too.
uint64_t heroSheathSentMs = 0;
bool heroWeaponsDrawn() {
    const uint64_t now = steadyMs();
    std::lock_guard<std::mutex> link(worldLinkMutex());
    auto &state = worldLink();
    const bool combat = (state.hud.unitFlags & 0x80000) != 0;
    const bool wanted = combat || (heroLastSwingMs && now - heroLastSwingMs < 6000);
    // CMSG_SETSHEATHED (u32 state) so the server and other players see the same weapons:
    // drawn for melee, put away a few seconds after the fight.
    const uint32_t sheath = wanted ? 1 : 0;
    if ((sheath != 0) != (state.hud.sheath != 0) && now - heroSheathSentMs > 2000 && state.outgoing.size() < 256) {
        heroSheathSentMs = now;
        WorldPacket packet;
        packet.opcode = 0x1E0;
        packet.payload = {uint8_t(sheath), 0, 0, 0};
        state.outgoing.push_back(std::move(packet));
    }
    return wanted || state.hud.sheath == 1;
}
bool heroInCombat() {
    std::lock_guard<std::mutex> link(worldLinkMutex());
    return (worldLink().hud.unitFlags & 0x80000) != 0; // UNIT_FLAG_IN_COMBAT
}
void heroAnimation() {
    auto *hero = scene.hero.get();
    if (!hero || !hero->dressed)
        return;
    const auto &p = scene.player;
    const bool moving = (p.moveX() != 0 || p.moveY() != 0) && !p.blocked();
    const auto &anims = hero->dressed->animations;
    uint16_t wanted = moving ? (p.backward() ? (anims.count(13) ? 13 : 4) : p.walking() ? 4 : 5) : 0;
    bool loop = true;
    uint32_t serial = 0;
    bool dead = false;
    {
        std::lock_guard<std::mutex> link(worldLinkMutex());
        const auto &hud = worldLink().hud;
        dead = hud.maxHealth && !hud.health;
    }
    if (dead) {
        // Death (1) plays once and holds its last frame; Dead (6) is the looping fallback.
        wanted = anims.count(1) ? 1 : anims.count(6) ? 6 : 0;
        loop = wanted != 1;
    }
    else if (p.swimming())
        wanted = moving ? (p.backward() && anims.count(45) ? 45 : anims.count(42) ? 42 : 5) : (anims.count(41) ? 41 : 0);
    else if (mountModel(scene.heroMount) && anims.count(91))
        wanted = 91; // riding
    else if (!p.onGround()) {
        if (anims.count(38))
            wanted = 38;
        else if (anims.count(37))
            wanted = 37;
        else if (anims.count(40))
            wanted = 40;
    } else if (const auto *action = activeAction(scene.self, steadyMs(), moving)) {
        wanted = action->anim;
        loop = action->loop;
        serial = action->serial;
        if (wanted >= 16 && wanted <= 18)
            heroLastSwingMs = steadyMs();
    }
    // In combat and standing: weapon ready stance between swings (Ready1H / ReadyUnarmed).
    if (wanted == 0 && !serial && heroInCombat()) {
        if (hero->dressed->mainHandType == 17 && anims.count(27))
            wanted = 27; // Ready2H
        else if (anims.count(26))
            wanted = 26;
        else if (anims.count(25))
            wanted = 25;
    }
    if (!anims.count(wanted))
        wanted = 0;
    const bool restart = serial && serial != scene.heroActionSerial;
    if ((wanted != scene.heroAnimation || restart) && anims.count(wanted)) {
        try {
            hero->dressed->body->play(anims.at(wanted), loop);
        } catch (const std::exception &) {
        }
        scene.heroAnimation = wanted;
    }
    scene.heroActionSerial = serial;
}
void drawDressed(UnitModel &model, M2ModelPreview &pose, const std::vector<C44Matrix> &worlds,
                 CShaderEffect &material, uint64_t now, uint32_t resampleMs, bool weaponsDrawn = false) {
    material.SetCurrent();
    auto &mesh = model.mesh(&pose);
    if (!resampleMs)
        mesh.animated = 1; // the hero switches clips on one pose: always re-skin
    pose.drawCached(mesh, worlds, scene.view, scene.projection, model.textures.get(), now, resampleMs);
    const auto &items = model.dressed->items;
    for (size_t i = 0; i < items.size(); ++i) {
        std::vector<C44Matrix> attached;
        attached.reserve(worlds.size());
        for (const auto &w : worlds)
            if (auto m = pose.model().attachment(
                    weaponsDrawn && items[i].drawnAttachment >= 0 ? uint32_t(items[i].drawnAttachment) : items[i].attachment, w))
                attached.push_back(*m);
        if (attached.empty())
            continue;
        material.SetCurrent();
        items[i].model->drawCached(model.mesh(items[i].model.get()), attached, scene.view, scene.projection,
                                   model.itemTextures[i].get(), now, 1000);
    }
}
} // namespace

void worldSceneAdopt() {
    WorldHandoff incoming;
    {
        std::lock_guard<std::mutex> lock(worldHandoffMutex());
        std::swap(incoming, worldHandoff());
    }
    for (auto &place : incoming.placements) {
        if (place->epoch < scene.epoch)
            continue;
        scene.active = true;
        if (place->epoch != scene.epoch || place->map != scene.map)
            clearMap();
        scene.epoch = place->epoch;
        scene.map = place->map;
        if (place->player) {
            scene.hero = adoptModel(std::move(place->player));
            scene.heroAnimation = 0xffff;
        }
        scene.pendingPlace = PlacementPack{place->epoch, place->map, place->x, place->y, place->z, place->facing, nullptr};
        scene.pendingSince = steadyMs();
        std::lock_guard<std::mutex> link(worldLinkMutex());
        worldLink().adoptedSerial = scene.epoch;
    }
    for (auto &tile : incoming.tiles) {
        if (tile->epoch != scene.epoch || !tile->terrain)
            continue;
        TileRender render;
        render.textures = std::make_unique<TerrainPreviewTextures>(tile->terrain->materials());
        scene.collision.addTerrain(tile->key, tile->terrain->surface());
        render.terrain = std::move(tile->terrain);
        buildWater(render);
        scene.tiles[tile->key] = std::move(render);
    }
    bool newFloors = false;
    for (auto &pack : incoming.objects) {
        if (pack->epoch != scene.epoch)
            continue;
        PackRender render;
        render.tile = pack->tile;
        for (auto &d : pack->doodads) {
            if (d.model) {
                auto &entry = scene.doodads[d.key];
                entry.textures = std::make_unique<M2PreviewTextures>(d.model->model().previewMaterials());
                entry.model = std::move(d.model);
            }
            render.doodads.push_back({d.key, std::move(d.instances)});
        }
        for (auto &object : pack->mapObjects) {
            MapObjectRender r;
            r.textures = std::make_unique<M2PreviewTextures>(object.model->materials());
            const auto &c = object.model->center();
            const auto &m = object.world;
            r.cx = c[0] * m.a0 + c[1] * m.b0 + c[2] * m.c0 + m.d0;
            r.cy = c[0] * m.a1 + c[1] * m.b1 + c[2] * m.c1 + m.d1;
            r.cz = c[0] * m.a2 + c[1] * m.b2 + c[2] * m.c2 + m.d2;
            r.radius = object.model->radius();
            r.world = object.world;
            r.model = std::move(object.model);
            render.mapObjects.push_back(std::move(r));
        }
        if (pack->collision && !pack->collision->empty()) {
            render.mesh = scene.nextMesh++;
            scene.collision.addMesh(render.mesh, pack->collision);
            newFloors = true;
        }
        scene.packs.push_back(std::move(render));
    }
    if (newFloors)
        scene.player.refloor(scene.collision);
    for (auto &unit : incoming.units) {
        scene.unitModels[unit->key] = adoptModel(std::move(unit->model));
    }
    // Pending placement: wait for ground at the server height (WMO floors may arrive after
    // the terrain); after 8 s accept any surface below.
    if (scene.pendingPlace) {
        const auto &p = *scene.pendingPlace;
        bool placed = false;
        if (scene.collision.groundBelow(p.x, p.y, p.z + 2) || steadyMs() - scene.pendingSince > 8000)
            placed = scene.player.place(scene.collision, p.x, p.y, p.z);
        if (placed) {
            scene.player.faceCamera(p.facing);
            scene.player.resetFrameClock();
            scene.pendingPlace.reset();
        }
    }
}
void worldSceneClear() {
    scene = Scene{};
    groundCache.clear();
    std::lock_guard<std::mutex> lock(worldHandoffMutex());
    worldHandoff() = WorldHandoff{};
    std::lock_guard<std::mutex> overlay(overlayMutex);
    overlayText.clear();
}
bool worldSceneActive() { return scene.active; }
namespace {
C44Matrix lookAtMatrix(const double eye[3], const double target[3]) {
    double f[3]{target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const double n = std::max(1e-6, std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]));
    for (double &v : f)
        v /= n;
    double r[3]{f[1], -f[0], 0};
    double rn = std::hypot(r[0], r[1]);
    if (rn < 1e-9) {
        r[0] = 1;
        r[1] = 0;
        rn = 1;
    }
    r[0] /= rn;
    r[1] /= rn;
    const double u[3]{r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    C44Matrix m(1);
    m.a0 = float(r[0]); m.b0 = float(r[1]); m.c0 = float(r[2]);
    m.a1 = float(u[0]); m.b1 = float(u[1]); m.c1 = float(u[2]);
    m.a2 = float(-f[0]); m.b2 = float(-f[1]); m.c2 = float(-f[2]);
    m.d0 = float(-(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]));
    m.d1 = float(-(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]));
    m.d2 = float(f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    return m;
}
C44Matrix perspectiveMatrix(double fovY, double aspect, double nearPlane, double farPlane) {
    const double f = 1.0 / std::tan(fovY / 2), a = aspect > 1e-3 ? aspect : 1;
    C44Matrix m(0);
    m.a0 = float(f / a);
    m.b1 = float(f);
    m.c2 = float(-(farPlane + nearPlane) / (farPlane - nearPlane));
    m.c3 = -1;
    m.d2 = float(-2 * farPlane * nearPlane / (farPlane - nearPlane));
    return m;
}
} // namespace
std::string viewError;
const std::string &worldSceneViewError() { return viewError; }
bool worldSceneDrawUnitView(uint64_t guid, float rotation, float aspect, bool portrait, float zoom) {
    if (!scene.active || !scene.material)
        return false;
    UnitModel *model = nullptr;
    M2ModelPreview *pose = nullptr;
    float scale = 1;
    uint64_t self = 0;
    {
        std::lock_guard<std::mutex> link(worldLinkMutex());
        self = worldLink().guid;
    }
    if (!guid || guid == self) {
        if (scene.hero && scene.hero->dressed && scene.hero->dressed->body) {
            model = scene.hero.get();
            pose = scene.hero->dressed->body.get();
        }
    } else {
        for (const auto &u : scene.units)
            if (u.guid == guid) {
                const auto found = scene.unitModels.find(u.modelKey);
                if (found != scene.unitModels.end() && found->second && found->second->dressed) {
                    model = found->second.get();
                    pose = model->pose(0);
                    scale = (u.scale > 0 ? u.scale : 1) * (model->dressed->scale > 0 ? model->dressed->scale : 1);
                }
                break;
            }
    }
    if (!model || !pose)
        return false;
    const C44Matrix world = unitMatrix(0, 0, 0, rotation, scale);
    const double *c = pose->boundsCenter();
    const double radius = std::max(0.3, pose->boundsRadius() * scale);
    double target[3]{0, 0, c[2] * scale};
    double distance = radius / std::tan(30 * 3.14159265358979 / 180) * 1.05 / std::max(0.2f, zoom);
    if (portrait) {
        // Head attachment (11) when the model has one, else the top of the bounds.
        if (auto head = pose->model().attachment(11, world)) {
            target[0] = head->d0;
            target[1] = head->d1;
            target[2] = head->d2;
        } else
            target[2] = (c[2] + pose->boundsRadius() * .7) * scale;
        distance = std::max(0.45, radius * 0.55);
    }
    double eye[3]{target[0] + distance, target[1], target[2] + (portrait ? 0.0 : radius * 0.1)};
    double fovy = portrait ? 0.9 : 1.0471975511965976;
    if (portrait && model->dressed->hasPortrait) {
        // The model's own portrait camera (M2Camera type 0), placed with the unit transform.
        const C3Vector e = model->dressed->portraitEye * world, t = model->dressed->portraitTarget * world;
        eye[0] = e.x; eye[1] = e.y; eye[2] = e.z;
        target[0] = t.x; target[1] = t.y; target[2] = t.z;
        fovy = model->dressed->portraitFov / std::sqrt(1.0 + double(aspect) * aspect); // stored FOV is diagonal
    }
    if (std::getenv("WOWPORT_PORTRAIT_DEBUG"))
        if (auto head = pose->model().attachment(11, world))
            std::fprintf(stderr, "PORTRAIT head %.2f %.2f %.2f eye %.2f %.2f %.2f target %.2f %.2f %.2f anim seq %u\n", head->d0, head->d1, head->d2,
                         eye[0], eye[1], eye[2], target[0], target[1], target[2], pose->model().sequence());
    const C44Matrix savedView = scene.view, savedProjection = scene.projection;
    scene.view = lookAtMatrix(eye, target);
    scene.projection = perspectiveMatrix(fovy, aspect, 0.05, 100);
    try {
        drawDressed(*model, *pose, {world}, *scene.material, scene.now, 1000);
        viewError.clear();
    } catch (const std::exception &error) {
        viewError = error.what();
    }
    scene.view = savedView;
    scene.projection = savedProjection;
    return true;
}
bool worldScenePlayerPose(uint32_t &map, double &x, double &y, double &z, double &heading) {
    if (!scene.active)
        return false;
    map = scene.map;
    x = scene.player.x();
    y = scene.player.y();
    z = scene.player.z();
    heading = scene.player.heading();
    return true;
}
void worldSceneOrbit(float dx, float dy) { scene.player.orbit(dx, dy); }
void worldSceneZoom(float factor) {
    float current = pendingZoom.load();
    while (!pendingZoom.compare_exchange_weak(current, current * factor)) {
    }
}
void worldSceneTap(float x, float y, float width, float height) {
    if (!scene.active || width <= 0 || height <= 0)
        return;
    const uint64_t now = steadyMs();
    uint64_t best = 0;
    bool bestObject = false;
    float bestDistance = 1e9f;
    for (const auto &u : scene.units) {
        const bool object = u.type == ObjectType::GameObject;
        const auto placed = placeUnit(u, now);
        const double range = std::hypot(placed.x - scene.player.x(), placed.y - scene.player.y());
        if (range > (object ? 25 : 60))
            continue;
        const auto found = scene.unitModels.find(u.modelKey);
        const float h = object ? 1.f : unitHeight(found == scene.unitModels.end() ? nullptr : found->second.get(), u);
        for (float k : {.25f, .55f, .9f}) {
            float sx, sy;
            if (!project(placed.x, placed.y, placed.z + h * k, sx, sy))
                continue;
            // Game objects (chests, herbs, quest objects) need a closer tap than creatures.
            const float d = std::hypot((sx - x / width) * width, (sy - y / height) * height) * (object ? 1.5f : 1.f);
            if (d < bestDistance) {
                bestDistance = d;
                best = u.guid;
                bestObject = object;
            }
        }
    }
    const float limit = std::max(48.f, std::min(width, height) * .09f);
    if (bestDistance > limit)
        best = 0;
    std::lock_guard<std::mutex> link(worldLinkMutex());
    auto &state = worldLink();
    // Tapping a game object uses it; tapping the selected unit again interacts (talk, loot, attack).
    if (best && (bestObject || best == scene.target)) {
        state.interact = best;
        ++state.interactSerial;
        if (bestObject)
            return;
    }
    scene.target = best;
    state.target = scene.target;
    ++state.targetSerial;
}
std::string worldSceneOverlay() {
    std::lock_guard<std::mutex> lock(overlayMutex);
    return overlayText;
}
// Desktop harness profiling (WOWPORT_PROFILE): GPU-synchronized section times.
struct SectionProfile {
    bool on = std::getenv("WOWPORT_PROFILE") != nullptr;
    std::chrono::steady_clock::time_point last;
    std::map<std::string, double> totals;
    int frames = 0;
    void start() { if (on) { glFinishHook(); last = std::chrono::steady_clock::now(); } }
    void mark(const char *name) {
        if (!on) return;
        glFinishHook();
        const auto now = std::chrono::steady_clock::now();
        totals[name] += std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
    }
    void end() {
        if (!on || ++frames % 20) return;
        std::string line = "PROFILE";
        for (const auto &[k, v] : totals) { char b[64]; std::snprintf(b, sizeof(b), " %s=%.1f", k.c_str(), v / 20); line += b; }
        std::fprintf(stderr, "%s\n", line.c_str());
        totals.clear();
    }
    static void glFinishHook() {}
} profile;
// Particles (CParticleEmitter2) of the doodads within 70 yards, one runtime per instance.
void drawDoodadParticles(const double focus[2], uint64_t now) {
    static uint64_t lastMs = 0;
    const float seconds = lastMs && now > lastMs ? std::min(.1f, float(now - lastMs) / 1000.f) : 0.f;
    lastMs = now;
    constexpr double range = 70;
    int budget = 48; // instances with live emitters per frame
    for (auto &[key, doodad] : scene.doodads) {
        const auto &effects = doodad.model ? doodad.model->effects() : nullptr;
        if (!effects || effects->particles.empty() || doodad.visible.empty())
            continue;
        std::vector<C44Matrix> local;
        for (const auto &m : doodad.visible) {
            const double dx = m.d0 - focus[0], dy = m.d1 - focus[1];
            if (dx * dx + dy * dy > range * range || budget <= 0)
                continue;
            --budget;
            const uint64_t id = (uint64_t(int64_t(std::lround(m.d0 * 4)) & 0x1fffff) << 42) ^
                                (uint64_t(int64_t(std::lround(m.d1 * 4)) & 0x1fffff) << 21) ^
                                uint64_t(int64_t(std::lround(m.d2 * 4)) & 0x1fffff);
            auto &slot = doodad.particles[id];
            if (!slot.runtime)
                slot.runtime = std::make_unique<M2ParticleRuntime>(effects);
            slot.seenMs = now;
            if (local.empty())
                local = doodad.model->model().bones();
            std::vector<C44Matrix> bones(local.size());
            for (size_t i = 0; i < local.size(); ++i)
                bones[i] = local[i] * m;
            const auto &playback = doodad.model->model().playback();
            try {
                slot.runtime->update(seconds, bones, playback.position(), playback.globalTime());
                slot.runtime->draw(scene.view, scene.projection);
            } catch (const std::exception &) {
            }
        }
        for (auto it = doodad.particles.begin(); it != doodad.particles.end();)
            it = now - it->second.seenMs > 5000 ? doodad.particles.erase(it) : std::next(it);
    }
}
void worldScenePaint(uint64_t milliseconds, float aspect, const WorldInput &input, CShaderEffect &material) {
    if (!scene.active)
        return;
    profile.start();
    scene.material = &material;
    scene.now = steadyMs();
    const float zoom = pendingZoom.exchange(1);
    if (zoom != 1)
        scene.player.zoom(zoom);
    scene.player.freeLook(input.freeLook);
    scene.player.setWater(liquidAt(scene.player.x(), scene.player.y()));
    scene.player.frame(scene.collision, milliseconds, input.right, input.forward, input.jump, input.walk);
    publishSample();
    heroAnimation();
    scene.aspect = aspect;
    scene.view = scene.player.view();
    scene.projection = scene.player.projection(aspect);
    const double focus[2]{scene.player.x(), scene.player.y()};
    constexpr double terrainRange = 400, terrainDetailRange = 170, objectRange = 380, doodadRange = 200;
    const uint64_t now = steadyMs();
    // Sky dome, fog and day/night tint (WorldLighting) for this frame.
    worldSkyMap(scene.map);
    profile.mark("player+anim");
    worldSkyDraw(scene.player.eye(),
                 C3Vector{float(scene.player.x()), float(scene.player.y()), float(scene.player.z())}, scene.view,
                 scene.projection, float(terrainRange - 40));
    profile.mark("sky");
    for (auto &[key, tile] : scene.tiles) {
        material.SetCurrent();
        tile.terrain->drawWith(scene.view, scene.projection, tile.textures.get(), false, true, focus, terrainRange,
                               tile.mesh.get(), terrainDetailRange);
    }
    profile.mark("sky+terrain");
    const Frustum frustum(scene.view * scene.projection);
    // WMOs.
    for (auto &pack : scene.packs)
        for (auto &object : pack.mapObjects) {
            const double d = std::hypot(object.cx - focus[0], object.cy - focus[1]) - object.radius;
            if (d > objectRange)
                continue;
            if (!frustum.sphere(object.cx, object.cy, object.cz, object.radius + 2.f))
                continue;
            material.SetCurrent();
            object.model->draw(object.world, scene.view, scene.projection, object.textures.get(), &object.meshes);
        }
    profile.mark("wmo");
    // Doodads: per model, visible instances across packs (static poses stay on the GPU).
    for (auto &[key, doodad] : scene.doodads)
        doodad.visible.clear();
    for (const auto &pack : scene.packs)
        for (const auto &[key, instances] : pack.doodads) {
            DoodadModel *target = nullptr;
            // Ground clutter (World\NoDXT\Detail) only near the player, like detailDoodadDist.
            const double range = key.rfind("world\\nodxt\\detail\\", 0) == 0 ? 42.0 : doodadRange;
            for (const auto &m : instances) {
                const double dx = m.d0 - focus[0], dy = m.d1 - focus[1];
                if (dx * dx + dy * dy >= range * range)
                    continue;
                if (!target) {
                    auto found = scene.doodads.find(key);
                    if (found == scene.doodads.end() || !found->second.model)
                        break;
                    target = &found->second;
                }
                const float scale = std::sqrt(m.a0 * m.a0 + m.a1 * m.a1 + m.a2 * m.a2);
                const double *c = target->model->boundsCenter();
                const float radius = float(target->model->boundsRadius()) * scale + 1.f;
                const C3Vector center = C3Vector{float(c[0]), float(c[1]), float(c[2])} * m;
                if (!frustum.sphere(center.x, center.y, center.z, radius))
                    continue;
                target->visible.push_back(m);
            }
        }
    for (auto &[key, doodad] : scene.doodads) {
        if (doodad.visible.empty())
            continue;
        doodad.model->frame(milliseconds);
        material.SetCurrent();
        doodad.model->drawCached(*doodad.mesh, doodad.visible, scene.view, scene.projection,
                                 doodad.textures.get(), now, 100);
    }
    profile.mark("doodads");
    drawDoodadParticles(focus, now);
    profile.mark("particles");
    // Units grouped by model and pose.
    std::map<std::pair<UnitModel *, uint32_t>, std::vector<C44Matrix>> groups;
    std::ostringstream overlay;
    overlay.setf(std::ios::fixed);
    overlay.precision(4);
    for (const auto &u : scene.units) {
        const auto placed = placeUnit(u, now);
        const double distance = std::hypot(placed.x - focus[0], placed.y - focus[1]);
        if (distance > 200)
            continue;
        auto found = scene.unitModels.find(u.modelKey);
        UnitModel *model = found == scene.unitModels.end() ? nullptr : found->second.get();
        if (model && model->dressed && u.mountDisplay && !u.dead)
            if (auto *mount = mountModel(u.mountDisplay)) {
                uint16_t mountAnim = placed.moving ? (u.walking ? 4 : 5) : 0;
                if (!mount->dressed->animations.count(mountAnim))
                    mountAnim = 0;
                const auto mountWorld = unitMatrix(placed.x, placed.y, placed.z, placed.facing, mount->dressed->scale);
                groups[{mount, mountAnim}].push_back(mountWorld);
                if (auto rider = riderMatrix(*mount, mountAnim, mountWorld, u.scale * model->dressed->scale)) {
                    const uint16_t riderAnim = model->dressed->animations.count(91) ? 91 : 0;
                    groups[{model, riderAnim}].push_back(*rider);
                }
                model = nullptr; // drawn mounted; nameplate below still uses the unit
            }
        if (model && model->dressed) {
            const auto world = unitMatrix(placed.x, placed.y, placed.z, placed.facing, u.scale * model->dressed->scale);
            const auto *action = u.dead ? nullptr : activeAction(u.guid, now, placed.moving);
            bool drawn = false;
            auto &alive = seenAlive[u.guid];
            if (u.dead && model->dressed->animations.count(1)) {
                // Death plays once from the moment the unit died and holds; a corpse first
                // seen already dead starts on the last frame.
                const bool fresh = !model->actions.count(u.guid) || model->actions[u.guid].anim != 1;
                if (auto *pose = model->action(u.guid, 1, false, fresh)) {
                    if (fresh && !alive)
                        pose->animated().seek(clipLength(model->dressed.get(), 1));
                    pose->frame(milliseconds);
                    drawDressed(*model, *pose, {world}, material, now, 33, u.sheath != 0);
                    drawn = true;
                }
            }
            if (!u.dead)
                alive = true;
            if (drawn) {
            } else if (action) {
                // Emote, swing or cast: the unit's own clip, started when the server said so.
                auto &seen = actionSerials[u.guid];
                const bool restart = seen != action->serial;
                seen = action->serial;
                if (auto *pose = model->action(u.guid, action->anim, action->loop, restart)) {
                    pose->frame(milliseconds);
                    drawDressed(*model, *pose, {world}, material, now, 33, u.sheath != 0);
                    drawn = true;
                }
            } else if (!model->actions.empty())
                model->dropAction(u.guid);
            uint16_t anim = u.dead ? 6 : placed.moving ? (u.walking ? 4 : 5) : 0;
            if (anim == 0 && (u.flags & 0x80000) && model->dressed->animations.count(26))
                anim = 26; // in combat: ready stance
            if (!model->dressed->animations.count(anim))
                anim = placed.moving && model->dressed->animations.count(4) ? 4 : 0;
            if (!drawn)
                groups[{model, uint32_t(anim) | (u.sheath ? 0x10000u : 0u)}].push_back(world);
        }
        // Quest giver mark on attachment 18 (above the head), PlayerNameComputeScale size.
        if (const uint32_t icon = questIcon(u.questStatus); icon && distance < 100)
            if (const auto iconModel = scene.unitModels.find("i:" + std::to_string(icon));
                iconModel != scene.unitModels.end() && iconModel->second->dressed) {
                const UnitModel *owner = found == scene.unitModels.end() ? nullptr : found->second.get();
                float height = unitHeight(owner, u) + .3f;
                C3Vector at{placed.x, placed.y, placed.z + height};
                if (owner && owner->dressed && owner->dressed->body) {
                    const auto world = unitMatrix(placed.x, placed.y, placed.z, placed.facing,
                                                  u.scale * owner->dressed->scale);
                    if (auto head = owner->dressed->body->model().attachment(18, world)) {
                        at = {head->d0, head->d1, head->d2};
                        height = at.z - placed.z;
                    }
                }
                const float scale = height <= 4 ? 1.f : height * .375f;
                groups[{iconModel->second.get(), 0}].push_back(unitMatrix(at.x, at.y, at.z, 0, scale));
            }
        // Nameplates for nearby units, and the target frame data.
        if (u.type != ObjectType::GameObject && (distance < 40 || u.guid == scene.target)) {
            float sx, sy;
            const float h = unitHeight(model, u);
            if (project(placed.x, placed.y, placed.z + h + .3f, sx, sy)) {
                std::string name = u.name.empty() ? "…" : u.name;
                std::replace(name.begin(), name.end(), '|', '/');
                std::replace(name.begin(), name.end(), '\n', ' ');
                char color[8];
                std::snprintf(color, sizeof(color), "%06x", nameColor(u, scene.heroFaction));
                overlay << "N|" << sx << '|' << sy << '|' << int(u.type == ObjectType::Player) << '|'
                        << int(u.dead) << '|' << int(u.guid == scene.target) << '|' << color << '|' << name << '\n';
            }
        }
        if (u.guid == scene.target) {
            std::string name = u.name;
            std::replace(name.begin(), name.end(), '|', '/');
            std::replace(name.begin(), name.end(), '\n', ' ');
            overlay << "T|" << u.level << '|' << u.health << '|' << u.maxHealth << '|' << u.power << '|'
                    << u.maxPower << '|' << int(u.powerType) << '|' << int(u.type == ObjectType::Player) << '|'
                    << name << '\n';
        }
    }
    profile.mark("unit-prep");
    for (auto &[key, worlds] : groups) {
        auto *pose = key.first->pose(uint16_t(key.second));
        if (!pose)
            continue;
        pose->frame(milliseconds);
        drawDressed(*key.first, *pose, worlds, material, now, 50, (key.second & 0x10000u) != 0);
    }
    profile.mark("units");
    // Local player.
    if (scene.hero && scene.hero->dressed && scene.player.placed()) {
        auto &body = *scene.hero->dressed->body;
        body.frame(milliseconds);
        auto world = unitMatrix(float(scene.player.x()), float(scene.player.y()), float(scene.player.z()),
                                float(scene.player.heading()), 1);
        if (auto *mount = mountModel(scene.heroMount)) {
            const auto &p = scene.player;
            const bool moving = (p.moveX() != 0 || p.moveY() != 0) && !p.blocked();
            uint16_t mountAnim = moving ? (p.backward() ? 13 : p.walking() ? 4 : 5) : 0;
            if (!mount->dressed->animations.count(mountAnim))
                mountAnim = moving && mount->dressed->animations.count(5) ? 5 : 0;
            const auto mountWorld = unitMatrix(float(p.x()), float(p.y()), float(p.z()), float(p.heading()),
                                               mount->dressed->scale);
            if (auto *pose = mount->pose(mountAnim)) {
                pose->frame(milliseconds);
                drawDressed(*mount, *pose, {mountWorld}, material, now, 33);
            }
            if (auto rider = riderMatrix(*mount, mountAnim, mountWorld, 1))
                world = *rider;
            if (scene.heroAnimation != 91 && scene.hero->dressed->animations.count(91)) {
                try {
                    body.play(scene.hero->dressed->animations.at(91), true);
                } catch (const std::exception &) {
                }
                scene.heroAnimation = 91;
            }
        }
        // Hide the body when the camera is inside it (first person zoom).
        const auto &eye = scene.player.eye();
        if (std::hypot(eye.x - scene.player.x(), eye.y - scene.player.y()) > .6)
            drawDressed(*scene.hero, body, {world}, material, now, 0, heroWeaponsDrawn());
    }
    profile.mark("hero");
    drawWater(focus, terrainRange);
    worldSkyEnd();
    profile.mark("water");
    profile.end();
    std::lock_guard<std::mutex> lock(overlayMutex);
    overlayText = overlay.str();
}
} // namespace wow::android
