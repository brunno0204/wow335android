// Asset worker for the in-world scene (MainActivity's single-thread executor): server
// placements and 3x3 ADT streaming (terrain, WMOs with their doodad sets, M2 doodads,
// collision), the player's dressed model and unit models requested by the GL thread.
#include "AssetLock.h"
#include "ClientData.h"
#include "GlueBridgeInternal.h"
#include "M2File.h"
#include "TerrainGeometry.h"
#include "TerrainObjects.h"
#include "WorldAssets.h"
#include "WorldLink.h"
#include "WorldScene.h"
#include "ClientDb.h"
#include "WoW/Source/WorldClient/WorldConstants.h"
#include <algorithm>
#include <android/log.h>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <atomic>
#include <deque>
#include <jni.h>
#include <set>
namespace wow::android {
std::mutex &worldHandoffMutex() {
    static std::mutex m;
    return m;
}
WorldHandoff &worldHandoff() {
    static WorldHandoff h;
    return h;
}
} // namespace wow::android
namespace {
using namespace wow::android;
using Clock = std::chrono::steady_clock;
struct Worker {
    uint32_t epoch = 0, map = 0;
    std::string mapDir;
    std::set<uint32_t> terrainDone, objectsDone, uids;
    std::map<uint32_t, std::map<std::string, std::vector<C44Matrix>>> clutter; // tile -> detail doodads
    std::map<uint32_t, std::vector<uint32_t>> tileUids;
    std::set<std::string> modelsSent, modelsFailed;
    std::map<std::string, CollisionShape> shapes;
    std::deque<std::string> unitQueue;
    std::set<std::string> unitKeys; // requested or loaded (GL keeps unit models)
    bool playerLoaded = false;
};
Worker worker;
std::atomic<size_t> queuedUnits{0}; // worker.unitQueue size, read by the network thread
std::map<uint32_t, std::string> mapDirectories;
std::string javaString(JNIEnv *env, jstring value) {
    if (!value)
        throw std::invalid_argument("Missing path");
    const char *bytes = env->GetStringUTFChars(value, nullptr);
    if (!bytes)
        throw std::runtime_error("Cannot access path");
    std::string result(bytes);
    env->ReleaseStringUTFChars(value, bytes);
    return result;
}
std::string lower(std::string s) {
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool superseded(uint32_t serial) {
    std::lock_guard<std::mutex> link(worldLinkMutex());
    const auto &state = worldLink();
    return !state.active || state.request.serial != serial;
}
void status(uint32_t serial, const std::string &text) {
    std::lock_guard<std::mutex> link(worldLinkMutex());
    auto &state = worldLink();
    if (state.request.serial == serial || state.loadedSerial < serial)
        state.loadedSerial = std::max(state.loadedSerial, serial);
    state.terrainStatus = text;
}
C44Matrix placementMatrix(const TerrainPlacementDef &def) {
    // CMap::CreateDoodadDef / CreateMapObjDef.
    C44Matrix m(1);
    m.d0 = def.position[0];
    m.d1 = def.position[1];
    m.d2 = def.position[2];
    m.RotateAroundZ(def.rotation[2] + 3.1415927f);
    m.RotateAroundY(def.rotation[1]);
    m.RotateAroundX(def.rotation[0]);
    if (def.scale != 1.f)
        m.Scale(def.scale);
    return m;
}
template <class T> void publish(std::vector<std::unique_ptr<T>> WorldHandoff::*list, std::unique_ptr<T> pack) {
    std::lock_guard<std::mutex> lock(worldHandoffMutex());
    (worldHandoff().*list).push_back(std::move(pack));
}
// Unit models requested by the GL thread; bounded by a time budget.
void serviceUnits(const std::string &directory, int budgetMs) {
    {
        std::lock_guard<std::mutex> link(worldLinkMutex());
        auto &state = worldLink();
        for (auto &key : state.modelRequests)
            if (worker.unitKeys.insert(key).second)
                worker.unitQueue.push_back(key);
        state.modelRequests.clear();
    }
    const auto end = Clock::now() + std::chrono::milliseconds(budgetMs);
    while (!worker.unitQueue.empty() && Clock::now() < end) {
        auto pack = std::make_unique<UnitModelPack>();
        pack->key = worker.unitQueue.front();
        worker.unitQueue.pop_front();
        std::string note;
        try {
            std::lock_guard<std::mutex> io(assetIoLock());
            const auto mount = sharedClientData(directory);
            pack->model = loadUnitModel(directory, pack->key, note);
        } catch (const std::exception &error) {
            __android_log_print(ANDROID_LOG_WARN, "WowPort", "Unit %s: %s", pack->key.c_str(), error.what());
        }
        publish(&WorldHandoff::units, std::move(pack));
    }
    queuedUnits.store(worker.unitQueue.size());
}
bool addDoodads(ObjectPack &pack, const std::string &path, const std::vector<C44Matrix> &instances);
// Ground clutter (CMapChunk::CreateDetailDoodads): per 8x8 cell the dominant layer's
// GroundEffectTexture picks weighted GroundEffectDoodad models, placed with the client's
// random scatter. Fewer per cell than the original density (phone budget); drawn near only.
struct GroundEffects {
    bool loaded = false;
    std::map<uint32_t, std::array<int32_t, 9>> textures; // id -> doodad[4], weight[4], density
    std::map<uint32_t, std::string> doodads;
} groundEffects;
void loadGroundEffects(const std::string &directory) {
    if (groundEffects.loaded)
        return;
    groundEffects.loaded = true;
    try {
        const auto mount = sharedClientData(directory);
        DbcTable textures(m2::readFile("DBFilesClient\\GroundEffectTexture.dbc", 8 * 1024 * 1024), 11, 44,
                          "GroundEffectTexture.dbc");
        for (uint32_t r = 0; r < textures.rows(); ++r) {
            std::array<int32_t, 9> row{};
            for (uint32_t k = 0; k < 9; ++k)
                row[k] = textures.i32(r, 1 + k);
            groundEffects.textures[textures.u32(r, 0)] = row;
        }
        DbcTable doodads(m2::readFile("DBFilesClient\\GroundEffectDoodad.dbc", 4 * 1024 * 1024), 3, 12,
                         "GroundEffectDoodad.dbc");
        for (uint32_t r = 0; r < doodads.rows(); ++r) {
            auto path = doodads.str(r, 1);
            if (path.empty())
                continue;
            const auto dot = path.find_last_of('.');
            if (dot != std::string::npos)
                path = path.substr(0, dot) + ".m2";
            groundEffects.doodads[doodads.u32(r, 0)] = "World\\NoDXT\\Detail\\" + path;
        }
    } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_WARN, "WowPort", "Ground effects: %s", error.what());
    }
}
std::map<std::string, std::vector<C44Matrix>> computeClutter(const TerrainPreview &terrain, uint32_t key) {
    constexpr uint32_t kPerCell = 3;
    const auto &g = terrain.geometry();
    const auto surface = terrain.surface();
    std::map<std::string, std::vector<C44Matrix>> byModel;
    uint32_t seed = 0x2545F491u ^ key;
    auto random = [&]() { // -1..1 (CRandom::sfloat_-like scatter)
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / float(1u << 24) * 2.f - 1.f;
    };
    for (const auto &chunk : g.chunks) {
        if (chunk.layers.empty())
            continue;
        const float top = HALF_WORLD_SIZE - float(g.tileY * 16 + chunk.y) * 33.333332f;
        const float left = HALF_WORLD_SIZE - float(g.tileX * 16 + chunk.x) * 33.333332f;
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x) {
                if (chunk.noEffectDoodad[y] & (1u << x))
                    continue;
                if (chunk.holes & (1u << ((y / 2) * 4 + x / 2)))
                    continue;
                const uint32_t layer = (chunk.lowQualityTexMap[y] >> (2 * x)) & 3;
                if (layer >= chunk.layers.size())
                    continue;
                const auto found = groundEffects.textures.find(chunk.layers[layer].effect);
                if (found == groundEffects.textures.end())
                    continue;
                const auto &e = found->second;
                int32_t total = e[4] + e[5] + e[6] + e[7];
                const uint32_t count = std::min<uint32_t>(kPerCell, e[8] > 0 ? uint32_t(std::max(1, e[8] / 6)) : 2);
                for (uint32_t j = 0; j < count; ++j) {
                    const float u = random(), v = random();
                    // Weighted doodad (weights sum to 16 in the client tables).
                    int32_t pick = total > 0 ? int32_t((random() * .5f + .5f) * float(total)) : 0;
                    int32_t id = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (!e[size_t(k)])
                            continue;
                        id = e[size_t(k)];
                        if (total <= 0 || pick < e[4 + size_t(k)])
                            break;
                        pick -= e[4 + size_t(k)];
                    }
                    const auto model = groundEffects.doodads.find(uint32_t(id));
                    if (model == groundEffects.doodads.end())
                        continue;
                    const float px = u * 2.0833333f + 2.0833333f, py = v * 2.0833333f + 2.0833333f;
                    const double wx = top - float(y) * 4.1666665f - py, wy = left - float(x) * 4.1666665f - px;
                    const auto hit = surface->heightAt(wx, wy);
                    if (!hit || hit->normal.z < .4f)
                        continue;
                    C44Matrix m(1);
                    m.d0 = float(wx);
                    m.d1 = float(wy);
                    m.d2 = float(hit->height);
                    m.RotateAroundZ((random() + 1.f) * 3.1415927f);
                    m.Scale(random() * .33f + 1.f);
                    byModel[model->second].push_back(m);
                }
            }
    }
    return byModel;
}
void sendClutter(const std::string &directory, uint32_t key) {
    const auto found = worker.clutter.find(key);
    if (found == worker.clutter.end())
        return;
    const auto byModel = std::move(found->second);
    worker.clutter.erase(found);
    if (byModel.empty())
        return;
    auto pack = std::make_unique<ObjectPack>();
    pack->epoch = worker.epoch;
    pack->tile = key;
    pack->collision = std::make_shared<CollisionMesh>();
    {
        std::lock_guard<std::mutex> io(assetIoLock());
        const auto mount = sharedClientData(directory);
        for (const auto &[path, instances] : byModel)
            addDoodads(*pack, path, instances);
    }
    if (pack->doodads.empty())
        return;
    pack->collision->build();
    publish(&WorldHandoff::objects, std::move(pack));
}
// Loads one ADT's terrain for the current epoch (missing tiles publish a null terrain).
void loadTerrainTile(const std::string &directory, uint32_t first, uint32_t second) {
    const uint32_t key = tileKey(first, second);
    if (worker.terrainDone.count(key))
        return;
    auto pack = std::make_unique<TerrainTilePack>();
    pack->epoch = worker.epoch;
    pack->key = key;
    try {
        std::lock_guard<std::mutex> io(assetIoLock());
        auto terrain = TerrainPreview::loadInstalled(directory, worker.mapDir, first, second);
        pack->terrain = std::make_unique<TerrainPreview>(std::move(terrain));
        loadGroundEffects(directory);
    } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_INFO, "WowPort", "ADT %u,%u: %s", first, second, error.what());
    }
    worker.terrainDone.insert(key);
    if (pack->terrain)
        try {
            worker.clutter[key] = computeClutter(*pack->terrain, key);
        } catch (const std::exception &) {
        }
    publish(&WorldHandoff::tiles, std::move(pack));
}
// Doodad model registry: the first pack that uses a model carries it to the GL thread.
bool addDoodads(ObjectPack &pack, const std::string &path, const std::vector<C44Matrix> &instances) {
    const auto key = lower(path);
    if (worker.modelsFailed.count(key) || instances.empty())
        return false;
    std::unique_ptr<M2ModelPreview> model;
    if (!worker.modelsSent.count(key)) {
        try {
            const auto bytes = m2::readFile(path);
            worker.shapes[key] = m2CollisionShape(bytes);
            model = std::make_unique<M2ModelPreview>(
                M2AnimatedModel::load(path, path.substr(0, path.size() - 3) + "00.skin", {0}, true));
            try { // animated UVs/colors and particle emitters (torches, fires, water)
                if (auto effects = loadM2Effects(bytes, 0); effects->animatedMaterials() || !effects->particles.empty())
                    model->setEffects(std::move(effects));
            } catch (const std::exception &) {
            }
            worker.modelsSent.insert(key);
        } catch (const std::exception &error) {
            worker.modelsFailed.insert(key);
            __android_log_print(ANDROID_LOG_WARN, "WowPort", "Doodad %s: %s", path.c_str(), error.what());
            return false;
        }
    }
    const auto &shape = worker.shapes[key];
    if (!shape.indices.empty())
        for (const auto &m : instances)
            pack.collision->append(shape, m);
    pack.doodads.push_back({key, std::move(model), instances});
    return true;
}
void loadTileObjects(const std::string &directory, uint32_t first, uint32_t second, uint32_t serial,
                     double px, double py) {
    const uint32_t key = tileKey(first, second);
    if (worker.objectsDone.count(key))
        return;
    TerrainObjects objects;
    try {
        std::lock_guard<std::mutex> io(assetIoLock());
        const auto mount = sharedClientData(directory);
        objects = decodeTerrainObjects(
            m2::readFile(terrainTilePath(worker.mapDir, first, second), 64 * 1024 * 1024));
    } catch (const std::exception &) {
        worker.objectsDone.insert(key);
        return;
    }
    auto distance = [&](const TerrainPlacementDef &d) {
        const double dx = d.position[0] - px, dy = d.position[1] - py;
        return dx * dx + dy * dy;
    };
    auto fresh = [&](const TerrainPlacementDef &d) {
        if (!worker.uids.insert(d.uniqueId).second)
            return false;
        worker.tileUids[key].push_back(d.uniqueId);
        return true;
    };
    auto newPack = [&] {
        auto pack = std::make_unique<ObjectPack>();
        pack->epoch = worker.epoch;
        pack->tile = key;
        pack->collision = std::make_shared<CollisionMesh>();
        return pack;
    };
    auto send = [&](std::unique_ptr<ObjectPack> pack) {
        pack->collision->build();
        publish(&WorldHandoff::objects, std::move(pack));
    };
    std::sort(objects.mapObjects.begin(), objects.mapObjects.end(),
              [&](const auto &a, const auto &b) { return distance(a) < distance(b); });
    for (const auto &def : objects.mapObjects) {
        if (superseded(serial))
            return;
        if (!fresh(def))
            continue;
        auto pack = newPack();
        try {
            std::lock_guard<std::mutex> io(assetIoLock());
            const auto mount = sharedClientData(directory);
            ObjectPack::MapObject object;
            object.model = std::make_unique<WmoModel>(WmoModel::load(def.path, def.doodadSet));
            object.world = placementMatrix(def);
            pack->collision->append(object.model->collision(), object.world);
            std::map<std::string, std::vector<C44Matrix>> inside;
            for (const auto &doodad : object.model->doodads())
                inside[doodad.path].push_back(doodad.local * object.world);
            for (const auto &[path, instances] : inside)
                addDoodads(*pack, path, instances);
            pack->mapObjects.push_back(std::move(object));
        } catch (const std::exception &error) {
            __android_log_print(ANDROID_LOG_WARN, "WowPort", "WMO %s: %s", def.path.c_str(), error.what());
            continue;
        }
        send(std::move(pack));
        serviceUnits(directory, 30);
    }
    // M2 doodads grouped by model, nearest model first.
    std::map<std::string, std::vector<const TerrainPlacementDef *>> byModel;
    std::map<std::string, double> nearest;
    for (const auto &def : objects.doodads) {
        if (worker.uids.count(def.uniqueId))
            continue;
        byModel[def.path].push_back(&def);
        const double d = distance(def);
        if (auto found = nearest.find(def.path); found == nearest.end() || d < found->second)
            nearest[def.path] = d;
    }
    std::vector<std::pair<double, std::string>> order;
    for (const auto &[path, d] : nearest)
        order.push_back({d, path});
    std::sort(order.begin(), order.end());
    auto pack = newPack();
    for (const auto &[d, path] : order) {
        if (superseded(serial))
            return;
        std::vector<C44Matrix> instances;
        for (const auto *def : byModel[path])
            if (fresh(*def))
                instances.push_back(placementMatrix(*def));
        {
            std::lock_guard<std::mutex> io(assetIoLock());
            const auto mount = sharedClientData(directory);
            addDoodads(*pack, path, instances);
        }
        if (pack->doodads.size() >= 16) {
            send(std::move(pack));
            pack = newPack();
            serviceUnits(directory, 20);
        }
    }
    if (!pack->doodads.empty())
        send(std::move(pack));
    worker.objectsDone.insert(key);
}
// WMO-only maps (dungeons, raids): WDT MPHD flag 1 with one MWMO/MODF global map object.
// Published once per epoch under a tile key no streaming eviction uses.
constexpr uint32_t kGlobalObjectTile = 64 * 64;
void loadGlobalObject(const std::string &directory, uint32_t serial) {
    if (worker.objectsDone.count(kGlobalObjectTile))
        return;
    worker.objectsDone.insert(kGlobalObjectTile);
    std::vector<uint8_t> wdt;
    try {
        std::lock_guard<std::mutex> io(assetIoLock());
        const auto mount = sharedClientData(directory);
        wdt = m2::readFile("World\\Maps\\" + worker.mapDir + "\\" + worker.mapDir + ".wdt", 4 * 1024 * 1024);
    } catch (const std::exception &) {
        return;
    }
    auto u32 = [&](size_t o) -> uint32_t {
        if (o + 4 > wdt.size())
            throw std::invalid_argument("Truncated WDT");
        return uint32_t(wdt[o]) | uint32_t(wdt[o + 1]) << 8 | uint32_t(wdt[o + 2]) << 16 | uint32_t(wdt[o + 3]) << 24;
    };
    try {
        uint32_t flags = 0;
        std::string name;
        size_t modf = 0;
        for (size_t at = 0; at + 8 <= wdt.size();) {
            const uint32_t id = u32(at), size = u32(at + 4);
            if (size > wdt.size() - at - 8)
                break;
            if (id == 0x4d504844) // MPHD
                flags = u32(at + 8);
            else if (id == 0x4d574d4f) // MWMO
                name.assign(reinterpret_cast<const char *>(wdt.data() + at + 8),
                            strnlen(reinterpret_cast<const char *>(wdt.data() + at + 8), size));
            else if (id == 0x4d4f4446 && size >= 64) // MODF
                modf = at + 8;
            at += 8 + size;
        }
        if (!(flags & 1) || name.empty() || !modf)
            return;
        // Same layout as the ADT MODF (CMap::CreateMapObjDef).
        auto f32 = [&](size_t o) {
            const uint32_t bits = u32(o);
            float v;
            std::memcpy(&v, &bits, 4);
            return std::isfinite(v) ? v : 0.f;
        };
        const float center = 32.f * (1600.f / 3.f);
        TerrainPlacementDef def;
        def.path = name;
        def.uniqueId = u32(modf + 4);
        def.position = {center - f32(modf + 16), center - f32(modf + 8), f32(modf + 12)};
        constexpr float rad = 0.017453292f;
        def.rotation = {f32(modf + 28) * rad, f32(modf + 20) * rad, f32(modf + 24) * rad};
        def.doodadSet = uint16_t(u32(modf + 56) >> 16);
        auto pack = std::make_unique<ObjectPack>();
        pack->epoch = worker.epoch;
        pack->tile = kGlobalObjectTile;
        pack->collision = std::make_shared<CollisionMesh>();
        {
            std::lock_guard<std::mutex> io(assetIoLock());
            const auto mount = sharedClientData(directory);
            ObjectPack::MapObject object;
            object.model = std::make_unique<WmoModel>(WmoModel::load(def.path, def.doodadSet));
            object.world = placementMatrix(def);
            pack->collision->append(object.model->collision(), object.world);
            std::map<std::string, std::vector<C44Matrix>> inside;
            for (const auto &doodad : object.model->doodads())
                inside[doodad.path].push_back(doodad.local * object.world);
            for (const auto &[path, instances] : inside)
                addDoodads(*pack, path, instances);
            pack->mapObjects.push_back(std::move(object));
        }
        pack->collision->build();
        publish(&WorldHandoff::objects, std::move(pack));
        (void)serial;
    } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_WARN, "WowPort", "Global WMO of %s: %s", worker.mapDir.c_str(), error.what());
    }
}
std::vector<std::pair<uint32_t, uint32_t>> neighbourhood(double x, double y) {
    const auto center = terrainTileFor(x, y);
    std::vector<std::pair<double, std::pair<uint32_t, uint32_t>>> tiles;
    constexpr double size = 1600.0 / 3.0;
    for (int a = -1; a <= 1; ++a)
        for (int b = -1; b <= 1; ++b) {
            const int first = int(center.first) + a, second = int(center.second) + b;
            if (first < 0 || second < 0 || first > 63 || second > 63)
                continue;
            // Tile centre in world space (first from Y, second from X).
            const double cy = (32 - first - .5) * size, cx = (32 - second - .5) * size;
            tiles.push_back({std::hypot(cx - x, cy - y), {uint32_t(first), uint32_t(second)}});
        }
    std::sort(tiles.begin(), tiles.end());
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (const auto &t : tiles)
        out.push_back(t.second);
    return out;
}
} // namespace

// Called on the asset worker whenever the snapshot reports a new placement/stream request
// or pending unit models. Returns a status text for a failed first placement, else null.
namespace wow::android {
// One asset-worker step (also driven by the desktop harness): empty, or a failure text.
std::string worldLoadStep(const std::string &directory) {
    WorldLinkState request;
    std::set<uint32_t> evicted;
    {
        std::lock_guard<std::mutex> link(worldLinkMutex());
        auto &state = worldLink();
        if (!state.active)
            return {};
        request.request = state.request;
        request.race = state.race;
        request.sex = state.sex;
        request.skin = state.skin;
        request.hairColor = state.hairColor;
        request.cls = state.cls;
        request.face = state.face;
        request.hairStyle = state.hairStyle;
        request.facialHair = state.facialHair;
        request.equipment = state.equipment;
        request.equipmentTypes = state.equipmentTypes;
        request.adoptedSerial = state.adoptedSerial;
        request.loadedSerial = state.loadedSerial;
        evicted.swap(state.evictedTiles);
    }
    for (uint32_t key : evicted) {
        worker.terrainDone.erase(key);
        worker.objectsDone.erase(key);
        for (uint32_t uid : worker.tileUids[key])
            worker.uids.erase(uid);
        worker.tileUids.erase(key);
    }
    const auto &place = request.request;
    if (!place.serial || place.serial <= request.loadedSerial) {
        serviceUnits(directory, 400);
        return {};
    }
    try {
        if (!place.stream || place.map != worker.map || !worker.epoch) {
            // New server placement: fresh epoch, center tile and (once) the player model.
            if (!request.adoptedSerial) // new session: the GL scene starts empty
                worker = Worker{};
            auto units = std::move(worker.unitQueue);
            auto unitKeys = std::move(worker.unitKeys);
            worker = Worker{};
            worker.unitQueue = std::move(units);
            worker.unitKeys = std::move(unitKeys);
            worker.epoch = place.serial;
            worker.map = place.map;
            std::unique_ptr<DressedModel> player;
            std::string note;
            {
                std::lock_guard<std::mutex> io(assetIoLock());
                const auto mount = sharedClientData(directory);
                m2::require(mount->report().ready, "Local game data is incomplete");
                {
                    std::lock_guard<std::mutex> link(worldLinkMutex());
                    worldLink().locale = mount->report().locale;
                }
                if (!mapDirectories.count(place.map))
                    mapDirectories[place.map] =
                        mapDirectory(m2::readFile("DBFilesClient\\Map.dbc", 16 * 1024 * 1024), place.map);
                worker.mapDir = mapDirectories.at(place.map);
                if (!request.adoptedSerial) {
                    try {
                        CharacterAppearance look;
                        look.race = request.race;
                        look.sex = request.sex;
                        look.skin = request.skin;
                        look.face = request.face;
                        look.hairStyle = request.hairStyle;
                        look.hairColor = request.hairColor;
                        look.facialHair = request.facialHair;
                        look.cls = request.cls;
                        look.displays = request.equipment;
                        look.inventoryTypes = request.equipmentTypes;
                        player = loadDressedCharacter(directory, look, {0, 5, 4, 13, 38, 40, 16, 17, 18, 26, 27, 52, 54, 32, 125, 9, 60, 66, 67, 68, 69, 70, 71, 72, 77, 80, 41, 42, 45, 6, 1, 91}, note);
                    } catch (const std::exception &error) {
                        note = std::string(" Modelo indisponível: ") + error.what() + ".";
                    }
                }
            }
            const auto tile = terrainTileFor(place.x, place.y);
            worker.playerLoaded = true;
            auto pack = std::make_unique<PlacementPack>();
            pack->epoch = place.serial;
            pack->map = place.map;
            pack->x = place.x;
            pack->y = place.y;
            pack->z = place.z;
            pack->facing = place.facing;
            pack->player = std::move(player);
            publish(&WorldHandoff::placements, std::move(pack));
            loadGlobalObject(directory, place.serial);
            loadTerrainTile(directory, tile.first, tile.second);
            status(place.serial, "Mapa carregado — bloco " + std::to_string(tile.first) + "," +
                                     std::to_string(tile.second) + "." + note);
        } else
            status(place.serial, "Carregando o mapa ao redor…");
        const auto tiles = neighbourhood(place.x, place.y);
        for (const auto &t : tiles) {
            if (superseded(place.serial))
                return {};
            loadTerrainTile(directory, t.first, t.second);
            serviceUnits(directory, 20);
        }
        for (const auto &t : tiles) {
            if (superseded(place.serial))
                return {};
            loadTileObjects(directory, t.first, t.second, place.serial, place.x, place.y);
            sendClutter(directory, tileKey(t.first, t.second));
        }
        serviceUnits(directory, 200);
        status(place.serial, "Mapa carregado.");
        return {};
    } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_ERROR, "WowPort", "World load: %s", error.what());
        const auto text = std::string("Não foi possível carregar o mapa ") + std::to_string(place.map) +
                          ": " + error.what();
        status(place.serial, text);
        return text;
    }
}
} // namespace wow::android
extern "C" JNIEXPORT jstring JNICALL
Java_org_wowdecomp_android_MainActivity_loadWorldTerrain(JNIEnv *env, jclass, jstring dataRoot) {
    std::string directory;
    try {
        directory = javaString(env, dataRoot);
    } catch (const std::exception &) {
        return nullptr;
    }
    const auto failure = wow::android::worldLoadStep(directory);
    if (failure.empty() || env->ExceptionCheck())
        return nullptr;
    return env->NewStringUTF(failure.c_str());
}
// Snapshot helper for NetworkBridge: asset work pending (unit models).
namespace wow::android {
size_t worldAssetWork() {
    std::lock_guard<std::mutex> link(worldLinkMutex());
    return worldLink().modelRequests.size() + queuedUnits.load();
}
} // namespace wow::android
