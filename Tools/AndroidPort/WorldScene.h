#pragma once
// In-world scene: asset-worker loading (WorldLoader.cpp) and GL-thread state/drawing
// (WorldScene.cpp). Worker -> GL handoff through owned packs under worldHandoffMutex().
#include "Engine/Source/Tempest/C44Matrix.h"
#include "M2ModelPreview.h"
#include "TerrainPreview.h"
#include "WmoModel.h"
#include "WorldCollision.h"
#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
class CShaderEffect;
namespace wow::android {
inline uint32_t tileKey(uint32_t first, uint32_t second) { return first * 64 + second; }
// Character/creature model with item models on M2 attachment points.
struct AttachedModel {
    std::unique_ptr<M2ModelPreview> model;
    uint32_t attachment = 0;       // sheathed point (back, hips) or the only point
    int32_t drawnAttachment = -1;  // weapons: hand point when drawn (sheath state melee/ranged)
};
struct DressedModel {
    std::unique_ptr<M2ModelPreview> body;
    std::vector<AttachedModel> items;
    std::map<uint16_t, uint32_t> animations; // animation ID -> preloaded slot
    uint8_t mainHandType = 0; // main hand inventory type (17 two-handed): melee/ready clips
    float scale = 1;
    // Portrait camera of the body model (model space, before scale), if it has one.
    bool hasPortrait = false;
    C3Vector portraitEye{}, portraitTarget{};
    float portraitFov = .7854f;
};
struct PlacementPack {
    uint32_t epoch = 0, map = 0;
    float x = 0, y = 0, z = 0, facing = 0;
    std::unique_ptr<DressedModel> player; // null: keep the current player model
};
struct TerrainTilePack {
    uint32_t epoch = 0, key = 0;
    std::unique_ptr<TerrainPreview> terrain; // null: no ADT here (ocean/void)
};
struct ObjectPack {
    uint32_t epoch = 0, tile = 0;
    struct Doodads {
        std::string key;                        // lowercase model path
        std::unique_ptr<M2ModelPreview> model;  // first use in this epoch only
        std::vector<C44Matrix> instances;
    };
    struct MapObject {
        std::unique_ptr<WmoModel> model;
        C44Matrix world{1};
    };
    std::vector<Doodads> doodads;
    std::vector<MapObject> mapObjects;
    std::shared_ptr<CollisionMesh> collision;
};
struct UnitModelPack {
    std::string key;
    std::unique_ptr<DressedModel> model; // null: load failed
};
struct WorldHandoff {
    std::vector<std::unique_ptr<PlacementPack>> placements;
    std::vector<std::unique_ptr<TerrainTilePack>> tiles;
    std::vector<std::unique_ptr<ObjectPack>> objects;
    std::vector<std::unique_ptr<UnitModelPack>> units;
};
// Asset worker step (WorldLoader.cpp): placement, tiles, objects and unit models; returns a
// failure text or empty.
std::string worldLoadStep(const std::string &directory);
std::mutex &worldHandoffMutex();
WorldHandoff &worldHandoff(); // guarded by worldHandoffMutex()

// Asset worker (GlueBridge.cpp): dressed character / creature / game object models.
// The caller holds the client data mount.
struct CharacterAppearance {
    uint8_t race = 0, sex = 0, skin = 0, face = 0, hairStyle = 0, hairColor = 0, facialHair = 0,
            cls = 0;
    std::array<uint32_t, 19> displays{}; // ItemDisplayInfo by equipment slot
    std::array<uint8_t, 19> inventoryTypes{};
    bool sheathed = true; // weapons on the back/hip instead of the hands
};
std::unique_ptr<DressedModel> loadDressedCharacter(const std::string &root, const CharacterAppearance &look,
                                                   const std::vector<uint16_t> &animations,
                                                   std::string &note);
// baked: CreatureDisplayInfoExtra BakeName texture replacing the composed skin.
std::unique_ptr<DressedModel> loadDressedCharacterWith(const std::string &root, const CharacterAppearance &look,
                                                       const std::vector<uint16_t> &animations,
                                                       std::string &note, const std::string &baked);
std::unique_ptr<DressedModel> loadUnitModel(const std::string &root, const std::string &key,
                                            std::string &note);

// GL thread.
struct WorldInput {
    double right = 0, forward = 0;
    bool jump = false, walk = false, freeLook = false;
};
void worldSceneAdopt();                 // drawFrame: take worker packs
void worldSceneClear();                 // leave world / GL teardown
bool worldSceneActive();
// Player position/heading (radians, 0 = +x north, counter-clockwise) for the minimap.
bool worldScenePlayerPose(uint32_t &map, double &x, double &y, double &z, double &heading);
// UI model frames: draws a unit (guid 0 / own GUID = local player) into the current Gx
// viewport, facing the camera after rotation (radians); portrait frames the head.
const std::string &worldSceneViewError(); // last unit view draw failure (empty: ok)
bool worldSceneDrawUnitView(uint64_t guid, float rotation, float aspect, bool portrait, float zoom);
void worldScenePaint(uint64_t milliseconds, float aspect, const WorldInput &input,
                     CShaderEffect &material);
void worldSceneOrbit(float dx, float dy);
void worldSceneZoom(float factor);
void worldSceneTap(float x, float y, float width, float height);
// Nameplates/target lines for the Java HUD ("N|x|y|player|dead|target|name", "T|level|hp|maxhp|
// power|maxpower|powertype|player|name"); screen coordinates normalized 0..1.
std::string worldSceneOverlay();
} // namespace wow::android
