#pragma once
#include "WorldWire.h"
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace wow::android {
// Item template (SMSG_ITEM_QUERY_SINGLE_RESPONSE, 3.3.5a ItemPrototype order).
struct ItemTemplate {
    uint32_t entry = 0, itemClass = 0, subClass = 0, display = 0, quality = 0, flags = 0, buyPrice = 0,
             sellPrice = 0, inventoryType = 0, itemLevel = 0, requiredLevel = 0, maxStack = 1,
             containerSlots = 0, armor = 0, delay = 0, bonding = 0, maxDurability = 0, damageType = 0;
    int32_t maxCount = 0;
    float damageMin = 0, damageMax = 0;
    std::string name, description;
    std::vector<std::pair<uint32_t, int32_t>> stats; // ItemModType, value
    std::array<uint32_t, 5> spells{}, spellTriggers{};
};
// Quest template (SMSG_QUEST_QUERY_RESPONSE, 3.3.5a QueryQuestInfo order).
struct QuestTemplate {
    uint32_t id = 0, type = 0, flags = 0, suggestedPlayers = 0, money = 0;
    int32_t level = 0, zoneOrSort = 0;
    std::string title, objectives, details, endText, completedText;
    std::array<int32_t, 4> creature{};
    std::array<uint32_t, 4> creatureCount{};
    std::array<uint32_t, 6> item{}, itemCount{};
    std::array<std::string, 4> objectiveText;
};
struct PartyMember {
    std::string name;
    uint64_t guid = 0;
    uint8_t online = 0, subGroup = 0, flags = 0, roles = 0;
};
struct SpellCooldown {
    uint32_t spell = 0, category = 0, cooldown = 0, categoryCooldown = 0;
};
// The player's update fields and owned item/container objects (network -> GL snapshot).
struct PlayerFieldsView {
    std::vector<uint32_t> fields;                    // player update fields
    std::map<uint64_t, std::vector<uint32_t>> items; // owned item/container fields by GUID
    uint32_t fieldsSerial = 0, itemsSerial = 0;
};
// Spells, action buttons, templates and group (network -> GL snapshot).
struct PlayerDataView {
    std::vector<uint32_t> spells;                        // known spell IDs (spellbook order)
    std::array<uint32_t, 144> actions{};                 // ACTION_BUTTON (type << 24 | id)
    std::map<uint32_t, ItemTemplate> itemTemplates;
    std::map<uint32_t, QuestTemplate> quests;
    std::map<uint32_t, std::string> creatureNames, gameObjectNames; // quest objective names
    std::vector<PartyMember> party;
    uint64_t leader = 0;
    uint8_t groupType = 0, lootMethod = 0, lootThreshold = 2;
    // Change counters per category: the UI fires matching script events.
    uint32_t spellsSerial = 0, actionsSerial = 0, questsSerial = 0, partySerial = 0, itemInfoSerial = 0;
};
// Network-thread state for the UI: spells, action buttons, quest and item templates, group.
class PlayerState {
  public:
    void reset() { *this = PlayerState{}; }
    // Returns true if the packet was one of ours. Replies (queries) go to out.
    bool handle(const WorldPacket &packet, std::vector<WorldPacket> &out);
    // Quest IDs from the quest log fields: queries for unknown ones go to out.
    void requestQuests(const std::vector<uint32_t> &playerFields, std::vector<WorldPacket> &out);
    void storeItemTemplate(ItemTemplate item);
    // Packets the UI sends that change local state the server does not echo
    // (CMSG_SET_ACTION_BUTTON).
    void noteOutgoing(const WorldPacket &packet);
    const std::map<uint32_t, ItemTemplate> &itemTemplates() const { return view_.itemTemplates; }
    const PlayerDataView &view() const { return view_; }
    // Server notifications as original script events (name + string arguments).
    std::vector<std::vector<std::string>> takeEvents() { return std::move(events_); }
    bool changed() const { return changed_; }
    void clearChanged() { changed_ = false; }

  private:
    PlayerDataView view_;
    std::vector<std::vector<std::string>> events_;
    std::set<uint32_t> askedQuests_, askedCreatures_, askedObjects_;
    bool changed_ = false;
};
// SMSG_ITEM_QUERY_SINGLE_RESPONSE payload -> template (entry 0 when unknown/invalid).
ItemTemplate parseItemTemplate(const std::vector<uint8_t> &payload);
} // namespace wow::android
