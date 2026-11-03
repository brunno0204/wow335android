#pragma once
#include "PlayerState.h"
#include "WorldWire.h"
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
namespace wow::android {
// Original 3.3.5a object types / update field indices (UpdateFields 12340).
enum class ObjectType : uint8_t { Object, Item, Container, Unit, Player, GameObject, DynamicObject, Corpse };
namespace field {
constexpr uint16_t OBJECT_ENTRY = 0x03, OBJECT_SCALE_X = 0x04;
constexpr uint16_t UNIT_TARGET = 0x12, UNIT_BYTES_0 = 0x17, UNIT_HEALTH = 0x18, UNIT_POWER1 = 0x19,
                   UNIT_MAXHEALTH = 0x20, UNIT_MAXPOWER1 = 0x21, UNIT_LEVEL = 0x36,
                   UNIT_FACTIONTEMPLATE = 0x37, UNIT_VIRTUAL_ITEM_SLOT_ID = 0x38, UNIT_FLAGS = 0x3b,
                   UNIT_DISPLAYID = 0x43, UNIT_MOUNTDISPLAYID = 0x45, UNIT_BYTES_1 = 0x4a,
                   UNIT_NPC_FLAGS = 0x52, UNIT_DYNAMIC_FLAGS = 0x4f, UNIT_BYTES_2 = 0x7a;
constexpr uint16_t PLAYER_BYTES = 0x99, PLAYER_BYTES_2 = 0x9a, PLAYER_BYTES_3 = 0x9b,
                   PLAYER_VISIBLE_ITEM_1_ENTRYID = 0x11b;
constexpr uint16_t GAMEOBJECT_DISPLAYID = 0x08, GAMEOBJECT_FLAGS = 0x09, GAMEOBJECT_BYTES_1 = 0x11;
} // namespace field
// Immutable per-unit view published by the network thread for the GL thread.
struct UnitView {
    uint64_t guid = 0;
    ObjectType type = ObjectType::Object;
    uint32_t entry = 0, displayId = 0, mountDisplay = 0, level = 0, health = 0, maxHealth = 0,
             power = 0, maxPower = 0, flags = 0, npcFlags = 0;
    uint8_t powerType = 0, race = 0, cls = 0, sex = 0;
    float scale = 1;
    // Position at timeMs (steady clock ms), optional linear path (monster spline) or
    // velocity extrapolation (player movement flags).
    float x = 0, y = 0, z = 0, facing = 0;
    std::vector<std::array<float, 3>> path; // path[0] = start
    uint64_t pathStartMs = 0;
    uint32_t pathDurationMs = 0;
    uint64_t timeMs = 0;
    float vx = 0, vy = 0;
    bool walking = false, moving = false, dead = false;
    uint8_t sheath = 0; // UNIT_FIELD_BYTES_2 byte 0: 1 melee, 2 ranged weapons drawn
    uint8_t questStatus = 0; // QuestGiverStatus (8 available '!', 10 reward '?', ...)
    uint32_t factionTemplate = 0;
    std::string name, subName;
    // Character appearance for players and humanoid NPCs: model key built by the loader.
    std::string modelKey;
};
// One-shot or timed animation of a unit (emote, melee swing, spell cast), network -> GL.
struct UnitAction {
    uint64_t guid = 0;
    uint16_t anim = 0;     // AnimationData ID
    uint32_t ms = 0;       // 0: the clip's own length
    bool loop = false;     // repeat until ms passes or the unit moves
};
struct HudState {
    uint32_t health = 0, maxHealth = 0, power = 0, maxPower = 0, level = 0;
    uint32_t mountDisplay = 0, unitFlags = 0;
    uint8_t sheath = 0;
    uint8_t powerType = 0;
    uint32_t factionTemplate = 0;
};
// Network-thread object manager: SMSG_(COMPRESSED_)UPDATE_OBJECT, DESTROY, monster
// moves, other movers and name/creature/item query caches. No combat/auras/inventory.
class WorldObjects {
  public:
    void reset(uint64_t self) {
        *this = WorldObjects{};
        self_ = self;
    }
    // Returns queries to send (name/creature/item). Unknown opcodes are ignored.
    bool handle(const WorldPacket &packet, uint64_t nowMs, std::vector<WorldPacket> &out);
    // Pending query packets (rate-limited); call after handle().
    void flushQueries(std::vector<WorldPacket> &out);
    bool changed() const noexcept { return changed_; }
    // Builds the snapshot and clears changed().
    std::vector<UnitView> snapshot(uint64_t nowMs);
    HudState hud() const;
    std::string nameOf(uint64_t guid) const;
    // What a tap on an object can do: type, NPC flags, dynamic flags (lootable), dead, position.
    struct Interaction {
        ObjectType type = ObjectType::Object;
        uint32_t npcFlags = 0, dynamicFlags = 0, unitFlags = 0, entry = 0;
        bool dead = false;
        float x = 0, y = 0, z = 0;
    };
    bool describe(uint64_t guid, Interaction &out) const;
    // Character equipment display IDs for a player by visible item entries (0 unknown).
    // Self falls back to the character-list display IDs.
    void setSelfEquipment(const std::array<uint32_t, 19> &displays) { selfEquipment_ = displays; }
    // UI data: the player's update fields and every item/container object (own inventory).
    void playerFields(PlayerFieldsView &view, bool items) const;
    // Self fields / owned items changed since the last call (cleared by it).
    bool takeSelfChanged() { const bool v = selfChanged_; selfChanged_ = false; return v; }
    bool takeItemsChanged() { const bool v = itemsChanged_; itemsChanged_ = false; return v; }
    // Queues an item query (UI asked for an item template it has not seen).
    void wantItem(uint32_t entry) {
        if (entry && !items_.count(entry) && askedItems_.insert(entry).second)
            itemQueue_.push_back(entry);
    }

  private:
    struct Object {
        ObjectType type = ObjectType::Object;
        std::vector<uint32_t> fields;
        float x = 0, y = 0, z = 0, facing = 0;
        std::vector<std::array<float, 3>> path;
        uint64_t pathStartMs = 0, timeMs = 0;
        uint32_t pathDurationMs = 0, moveFlags = 0;
        float runSpeed = 7, walkSpeed = 2.5f;
        bool walking = false;
        uint8_t questStatus = 0; // SMSG_QUESTGIVER_STATUS(_MULTIPLE)
        uint32_t get(uint16_t index) const { return index < fields.size() ? fields[index] : 0; }
    };
    void update(const std::vector<uint8_t> &payload, uint64_t nowMs);
    void readMovement(Object &object, const std::vector<uint8_t> &p, size_t &o, uint64_t nowMs);
    void readValues(Object &object, const std::vector<uint8_t> &p, size_t &o);
    void moved(uint64_t guid, const std::vector<uint8_t> &p, size_t &o, uint64_t nowMs);
    void monsterMove(const std::vector<uint8_t> &p, uint64_t nowMs);
    void request(const Object &object, uint64_t guid);
    void noteChange(const Object &object, uint64_t guid);
    std::array<uint32_t, 19> equipment(uint64_t guid, const Object &object);
    uint64_t self_ = 0;
    std::unordered_map<uint64_t, Object> objects_;
    std::map<uint64_t, std::string> playerNames_;
    struct CreatureInfo {
        std::string name, subName;
    };
    std::map<uint32_t, CreatureInfo> creatures_;
    struct ItemInfo {
        uint32_t display = 0, inventoryType = 0;
    };
    std::map<uint32_t, ItemInfo> items_;
    std::set<uint64_t> askedNames_;
    std::set<uint32_t> askedCreatures_, askedItems_;
    std::vector<uint64_t> nameQueue_;
    std::vector<uint32_t> creatureQueue_, itemQueue_;
    std::array<uint32_t, 19> selfEquipment_{};
    bool changed_ = false, selfChanged_ = false, itemsChanged_ = false;
};
} // namespace wow::android
