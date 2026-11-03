#include "PlayerState.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace wow::android {
namespace {
constexpr uint32_t SMSG_ACTION_BUTTONS = 0x129, SMSG_INITIAL_SPELLS = 0x12a, SMSG_LEARNED_SPELL = 0x12b,
                   SMSG_SUPERCEDED_SPELL = 0x12c, SMSG_REMOVED_SPELL = 0x203, SMSG_GROUP_LIST = 0x07d,
                   SMSG_GROUP_DESTROYED = 0x07c, SMSG_QUEST_QUERY_RESPONSE = 0x05d, CMSG_QUEST_QUERY = 0x05c,
                   PLAYER_QUEST_LOG_1_1 = 0x9e, SMSG_CREATURE_QUERY_RESPONSE = 0x061,
                   CMSG_CREATURE_QUERY = 0x060, CMSG_GAMEOBJECT_QUERY = 0x05e,
                   SMSG_GAMEOBJECT_QUERY_RESPONSE = 0x05f, SMSG_GROUP_INVITE = 0x06f,
                   SMSG_NOTIFICATION = 0x1cb, SMSG_CAST_FAILED = 0x130, SMSG_GROUP_DECLINE = 0x074,
                   SMSG_GROUP_UNINVITE = 0x077, SMSG_PARTY_COMMAND_RESULT = 0x07f;
struct Reader {
    const std::vector<uint8_t> &p;
    size_t o = 0;
    void need(size_t n) const {
        if (o > p.size() || p.size() - o < n)
            throw std::invalid_argument("truncated packet");
    }
    uint8_t u8() { need(1); return p[o++]; }
    uint16_t u16() { need(2); const uint16_t v = uint16_t(p[o] | p[o + 1] << 8); o += 2; return v; }
    uint32_t u32() {
        need(4);
        const uint32_t v = uint32_t(p[o]) | uint32_t(p[o + 1]) << 8 | uint32_t(p[o + 2]) << 16 | uint32_t(p[o + 3]) << 24;
        o += 4;
        return v;
    }
    int32_t i32() { return int32_t(u32()); }
    uint64_t u64() { const uint64_t low = u32(); return low | uint64_t(u32()) << 32; }
    float f32() {
        const uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, 4);
        return v == v ? v : 0.f;
    }
    std::string str() {
        const size_t start = o;
        while (o < p.size() && p[o])
            ++o;
        need(1);
        std::string s(p.begin() + long(start), p.begin() + long(o));
        ++o;
        return s;
    }
};
void put32(std::vector<uint8_t> &p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p.push_back(uint8_t(v >> (8 * i)));
}
} // namespace

ItemTemplate parseItemTemplate(const std::vector<uint8_t> &payload) {
    Reader r{payload};
    ItemTemplate t;
    const uint32_t entry = r.u32();
    if (entry & 0x80000000u)
        return {};
    t.itemClass = r.u32();
    t.subClass = r.u32();
    r.u32(); // sound override subclass
    t.name = r.str();
    for (int i = 0; i < 3; ++i)
        r.str();
    t.display = r.u32();
    t.quality = r.u32();
    t.flags = r.u32();
    r.u32(); // flags2
    t.buyPrice = r.u32();
    t.sellPrice = r.u32();
    t.inventoryType = r.u32();
    r.u32(); // allowable class
    r.u32(); // allowable race
    t.itemLevel = r.u32();
    t.requiredLevel = r.u32();
    for (int i = 0; i < 7; ++i)
        r.u32(); // skill, skill rank, spell, honor rank, city rank, reputation faction/rank
    t.maxCount = r.i32();
    t.maxStack = std::max<int32_t>(1, r.i32());
    t.containerSlots = r.u32();
    const uint32_t stats = r.u32();
    for (uint32_t i = 0; i < stats && i < 10; ++i) {
        const uint32_t type = r.u32();
        const int32_t value = r.i32();
        t.stats.emplace_back(type, value);
    }
    r.u32(); // scaling stat distribution
    r.u32(); // scaling stat value
    for (int i = 0; i < 2; ++i) {
        const float minimum = r.f32(), maximum = r.f32();
        const uint32_t type = r.u32();
        if (i == 0) {
            t.damageMin = minimum;
            t.damageMax = maximum;
            t.damageType = type;
        }
    }
    t.armor = r.u32();
    for (int i = 0; i < 6; ++i)
        r.u32(); // resistances
    t.delay = r.u32();
    r.u32();  // ammo type
    r.f32();  // ranged range modifier
    for (int i = 0; i < 5; ++i) {
        const int32_t spell = r.i32();
        t.spells[size_t(i)] = spell > 0 ? uint32_t(spell) : 0;
        t.spellTriggers[size_t(i)] = r.u32();
        for (int k = 0; k < 4; ++k)
            r.u32(); // charges, cooldown, category, category cooldown
    }
    t.bonding = r.u32();
    t.description = r.str();
    for (int i = 0; i < 11; ++i)
        r.u32(); // page text .. item set (page, language, material, start quest, lock, material,
                 // sheath, random property, random suffix, block, item set)
    t.maxDurability = r.u32();
    t.entry = entry;
    return t;
}

bool PlayerState::handle(const WorldPacket &packet, std::vector<WorldPacket> &out) {
    Reader r{packet.payload};
    try {
        switch (packet.opcode) {
        case SMSG_INITIAL_SPELLS: {
            r.u8();
            const uint16_t count = r.u16();
            view_.spells.clear();
            for (uint16_t i = 0; i < count; ++i) {
                view_.spells.push_back(r.u32());
                r.u16();
            }
            ++view_.spellsSerial;
            break;
        }
        case SMSG_LEARNED_SPELL: {
            const uint32_t spell = r.u32();
            if (std::find(view_.spells.begin(), view_.spells.end(), spell) == view_.spells.end())
                view_.spells.push_back(spell);
            ++view_.spellsSerial;
            break;
        }
        case SMSG_REMOVED_SPELL: {
            const uint32_t spell = r.u32();
            view_.spells.erase(std::remove(view_.spells.begin(), view_.spells.end(), spell), view_.spells.end());
            ++view_.spellsSerial;
            break;
        }
        case SMSG_SUPERCEDED_SPELL: {
            const uint32_t old = r.u32(), replacement = r.u32();
            auto found = std::find(view_.spells.begin(), view_.spells.end(), old);
            if (found != view_.spells.end())
                *found = replacement;
            else
                view_.spells.push_back(replacement);
            ++view_.spellsSerial;
            break;
        }
        case SMSG_ACTION_BUTTONS: {
            const uint8_t state = r.u8();
            if (state == 2)
                view_.actions.fill(0);
            else
                for (auto &action : view_.actions)
                    action = r.u32();
            ++view_.actionsSerial;
            break;
        }
        case SMSG_GROUP_DESTROYED:
            view_.party.clear();
            view_.leader = 0;
            view_.groupType = 0;
            ++view_.partySerial;
            break;
        case SMSG_GROUP_LIST: {
            view_.groupType = r.u8();
            r.u8(); // own subgroup
            r.u8(); // own flags
            r.u8(); // own roles
            if (view_.groupType & 0x08) { // LFG
                r.u8();
                r.u32();
            }
            r.u64(); // group GUID
            r.u32(); // counter
            const uint32_t count = r.u32();
            view_.party.clear();
            for (uint32_t i = 0; i < count && i < 40; ++i) {
                PartyMember m;
                m.name = r.str();
                m.guid = r.u64();
                m.online = r.u8();
                m.subGroup = r.u8();
                m.flags = r.u8();
                m.roles = r.u8();
                view_.party.push_back(std::move(m));
            }
            view_.leader = r.u64();
            if (count) {
                view_.lootMethod = r.u8();
                r.u64();
                view_.lootThreshold = r.u8();
            }
            ++view_.partySerial;
            break;
        }
        case SMSG_QUEST_QUERY_RESPONSE: {
            QuestTemplate q;
            q.id = r.u32();
            r.u32(); // method
            q.level = r.i32();
            r.u32(); // min level
            q.zoneOrSort = r.i32();
            q.type = r.u32();
            q.suggestedPlayers = r.u32();
            for (int i = 0; i < 4; ++i)
                r.u32(); // reputation objectives
            r.u32();     // next quest in chain
            r.u32();     // xp id
            q.money = r.u32();
            for (int i = 0; i < 4; ++i)
                r.u32(); // money max level, reward spell, spell cast, honor
            r.f32();     // honor multiplier
            r.u32();     // source item
            q.flags = r.u32();
            for (int i = 0; i < 5; ++i)
                r.u32(); // title, players slain, bonus talents, arena points, unknown
            for (int i = 0; i < 8; ++i)
                r.u32(); // reward items + counts
            for (int i = 0; i < 12; ++i)
                r.u32(); // choice items + counts
            for (int i = 0; i < 15; ++i)
                r.u32(); // reputation rewards
            r.u32();     // point map
            r.f32();
            r.f32();
            r.u32();     // point option
            q.title = r.str();
            q.objectives = r.str();
            q.details = r.str();
            q.endText = r.str();
            q.completedText = r.str();
            for (size_t i = 0; i < 4; ++i) {
                q.creature[i] = r.i32();
                q.creatureCount[i] = r.u32();
                r.u32(); // source item
                r.u32(); // source count
            }
            for (size_t i = 0; i < 6; ++i) {
                q.item[i] = r.u32();
                q.itemCount[i] = r.u32();
            }
            for (size_t i = 0; i < 4; ++i)
                q.objectiveText[i] = r.str();
            view_.quests[q.id] = std::move(q);
            ++view_.questsSerial;
            break;
        }
        case SMSG_GROUP_INVITE: {
            const uint8_t status = r.u8();
            const std::string name = r.str();
            if (status)
                events_.push_back({"PARTY_INVITE_REQUEST", name});
            return true;
        }
        case SMSG_GROUP_DECLINE:
            events_.push_back({"__ANDROID_GROUP_DECLINE", r.str()});
            return true;
        case SMSG_GROUP_UNINVITE:
            view_.party.clear();
            view_.leader = 0;
            ++view_.partySerial;
            break;
        case SMSG_PARTY_COMMAND_RESULT: {
            const uint32_t operation = r.u32();
            const std::string name = r.str();
            const uint32_t result = r.u32();
            if (result)
                events_.push_back({"__ANDROID_PARTY_RESULT", std::to_string(operation), name, std::to_string(result)});
            return true;
        }
        case SMSG_NOTIFICATION:
            events_.push_back({"UI_INFO_MESSAGE", r.str()});
            return true;
        case SMSG_CAST_FAILED: {
            r.u8();
            const uint32_t spell = r.u32();
            const uint8_t result = r.u8();
            events_.push_back({"__ANDROID_CAST_FAILED", std::to_string(spell), std::to_string(result)});
            return true;
        }
        case SMSG_CREATURE_QUERY_RESPONSE: {
            const uint32_t entry = r.u32();
            if (entry & 0x80000000u)
                return false;
            view_.creatureNames[entry] = r.str();
            ++view_.questsSerial;
            break;
        }
        case SMSG_GAMEOBJECT_QUERY_RESPONSE: {
            const uint32_t entry = r.u32();
            if (entry & 0x80000000u)
                return false;
            r.u32(); // type
            r.u32(); // display
            view_.gameObjectNames[entry] = r.str();
            ++view_.questsSerial;
            break;
        }
        default:
            return false;
        }
    } catch (const std::exception &) {
        // Malformed or different server layout: keep what was read before.
    }
    if (packet.opcode == SMSG_QUEST_QUERY_RESPONSE)
        for (auto &[id, quest] : view_.quests)
            for (size_t i = 0; i < 4; ++i) {
                const int32_t target = quest.creature[i];
                if (target > 0 && !view_.creatureNames.count(uint32_t(target)) &&
                    askedCreatures_.insert(uint32_t(target)).second) {
                    WorldPacket query{CMSG_CREATURE_QUERY, {}};
                    put32(query.payload, uint32_t(target));
                    put32(query.payload, 0);
                    put32(query.payload, 0);
                    out.push_back(std::move(query));
                } else if (target < 0 && !view_.gameObjectNames.count(uint32_t(-target)) &&
                           askedObjects_.insert(uint32_t(-target)).second) {
                    WorldPacket query{CMSG_GAMEOBJECT_QUERY, {}};
                    put32(query.payload, uint32_t(-target));
                    put32(query.payload, 0);
                    put32(query.payload, 0);
                    out.push_back(std::move(query));
                }
            }
    changed_ = true;
    return true;
}
void PlayerState::requestQuests(const std::vector<uint32_t> &fields, std::vector<WorldPacket> &out) {
    for (uint32_t slot = 0; slot < 25; ++slot) {
        const uint32_t index = PLAYER_QUEST_LOG_1_1 + slot * 5;
        const uint32_t quest = index < fields.size() ? fields[index] : 0;
        if (quest && !view_.quests.count(quest) && askedQuests_.insert(quest).second) {
            WorldPacket query{CMSG_QUEST_QUERY, {}};
            put32(query.payload, quest);
            out.push_back(std::move(query));
        }
    }
}
void PlayerState::noteOutgoing(const WorldPacket &packet) {
    if (packet.opcode == 0x128 && packet.payload.size() >= 5) { // CMSG_SET_ACTION_BUTTON
        const uint8_t button = packet.payload[0];
        const uint32_t value = uint32_t(packet.payload[1]) | uint32_t(packet.payload[2]) << 8 |
                               uint32_t(packet.payload[3]) << 16 | uint32_t(packet.payload[4]) << 24;
        if (button < view_.actions.size()) {
            view_.actions[button] = value;
            ++view_.actionsSerial;
            changed_ = true;
        }
    }
}
void PlayerState::storeItemTemplate(ItemTemplate item) {
    if (!item.entry)
        return;
    view_.itemTemplates[item.entry] = std::move(item);
    ++view_.itemInfoSerial;
    changed_ = true;
}
} // namespace wow::android
