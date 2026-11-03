#pragma once
#include "WorldPresence.h"
#include "WorldObjects.h"
#include <array>
#include <memory>
#include <set>
#include <vector>
#include <mutex>
#include <string>
namespace wow::android {
// Process-wide handoff between the Java network executor (WorldPresence owner),
// the asset worker (terrain loads) and the GL frame owner (local mover). Short
// critical sections only; no IO, GL or socket work is done while holding it.
// One received chat line (SMSG_MESSAGECHAT / SMSG_GM_MESSAGECHAT / SMSG_MOTD).
struct ChatLine {
    uint8_t type = 0;
    uint32_t language = 0;
    uint64_t sender = 0;
    std::string author, channel, text;
};
struct WorldLinkState {
    bool active = false;
    uint64_t guid = 0;
    uint8_t race = 0, sex = 0, skin = 0, hairColor = 0, cls = 0, face = 0, hairStyle = 0,
            facialHair = 0;
    TerrainPlacement request;     // newest placement to load (server or tile edge)
    uint32_t nextSerial = 0;      // allocator shared by server and local requests
    uint32_t adoptedSerial = 0;   // placement currently simulated by the GL thread
    uint32_t loadedSerial = 0;    // last placement the worker finished (or failed)
    MoverSample sample;           // latest GL sample
    uint32_t sampleSerial = 0;    // placement the sample was simulated on
    uint32_t requiredSerial = 0;  // newest server relocation; older samples are stale
    std::string terrainStatus;
    std::array<uint32_t, 19> equipment{}; // hero ItemDisplayInfo IDs by equipment slot
    std::array<uint8_t, 19> equipmentTypes{}; // hero inventory types (weapon grips)
    float runSpeed = 7;
    // Network -> GL: visible objects and the player HUD.
    std::vector<UnitView> units;
    uint32_t unitsSerial = 0;
    HudState hud;
    std::string playerName, realmName, locale;
    std::vector<ChatLine> chat; // network -> UI, drained by the UI
    uint32_t mapId = 0, zoneId = 0;
    uint32_t areaId = 0;
    uint32_t gameHalfMinutes = 0, gameTimeSerial = 0; // network -> GL (sky) // GL: AreaTable ID of the terrain under the player (zone/subzone text)
    // GL -> network: selection (CMSG_SET_SELECTION) when targetSerial changes.
    uint64_t target = 0;
    uint32_t targetSerial = 0;
    // GL/UI -> network: interact with an object (tap on the selected unit, a game object, InteractUnit).
    uint64_t interact = 0;
    uint32_t interactSerial = 0;
    // Network -> UI: the player's data for the original interface (immutable snapshots).
    std::shared_ptr<const PlayerFieldsView> playerFields;
    std::shared_ptr<const PlayerDataView> playerData;
    // UI -> network: packets to send (chat, casts, action buttons...) and item templates wanted.
    std::vector<WorldPacket> outgoing;
    // Network -> UI: script events (name, string arguments) such as PARTY_INVITE_REQUEST.
    std::vector<std::vector<std::string>> scriptEvents;
    std::vector<uint32_t> wantItems;
    // Network -> GL: unit animations (emotes, swings, casts).
    std::vector<UnitAction> unitActions;
    // GL -> asset worker: unit model keys to load; tiles dropped from the scene.
    std::vector<std::string> modelRequests;
    std::set<uint32_t> evictedTiles;
};
inline std::mutex &worldLinkMutex() {
    static std::mutex mutex;
    return mutex;
}
inline WorldLinkState &worldLink() {
    static WorldLinkState state;
    return state;
}
} // namespace wow::android
