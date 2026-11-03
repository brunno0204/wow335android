#include "ServerEvents.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
namespace wow::android {
namespace {
// Opcodes (WoW/Source/Net/Types.h, 3.3.5a build 12340).
enum : uint32_t {
    SMSG_WHO = 0x063, SMSG_CONTACT_LIST = 0x067, SMSG_FRIEND_STATUS = 0x068, SMSG_QUERY_GUILD_INFO_RESPONSE = 0x055,
    SMSG_GUILD_ROSTER = 0x08a, SMSG_GUILD_EVENT = 0x092, SMSG_GUILD_INVITE = 0x083,
    SMSG_EMOTE = 0x103, SMSG_TEXT_EMOTE = 0x105, SMSG_INVENTORY_CHANGE_FAILURE = 0x112,
    SMSG_SPELL_START = 0x131, SMSG_SPELL_GO = 0x132, SMSG_SPELL_FAILURE = 0x133, SMSG_SPELL_COOLDOWN = 0x134,
    SMSG_COOLDOWN_EVENT = 0x135, MSG_CHANNEL_START = 0x139, MSG_CHANNEL_UPDATE = 0x13a,
    SMSG_ATTACK_START = 0x143, SMSG_ATTACK_STOP = 0x144, SMSG_ATTACKER_STATE_UPDATE = 0x14a,
    SMSG_SPELL_HEAL_LOG = 0x150, SMSG_RESURRECT_REQUEST = 0x15b, SMSG_LOOT_RESPONSE = 0x160,
    SMSG_LOOT_RELEASE = 0x161, SMSG_LOOT_REMOVED = 0x162, SMSG_LOOT_MONEY_NOTIFY = 0x163,
    SMSG_LOOT_CLEAR_MONEY = 0x165, SMSG_ITEM_PUSH_RESULT = 0x166, SMSG_GOSSIP_MESSAGE = 0x17d,
    SMSG_GOSSIP_COMPLETE = 0x17e, CMSG_QUERY_NPC_TEXT = 0x17f, SMSG_QUERY_NPC_TEXT_RESPONSE = 0x180,
    SMSG_QUEST_GIVER_QUEST_LIST_MESSAGE = 0x185, SMSG_QUEST_GIVER_QUEST_DETAILS = 0x188,
    SMSG_QUEST_GIVER_REQUEST_ITEMS = 0x18b, SMSG_QUEST_GIVER_OFFER_REWARD_MESSAGE = 0x18d,
    SMSG_QUEST_GIVER_INVALID_QUEST = 0x18f, SMSG_QUEST_GIVER_QUEST_COMPLETE = 0x191,
    SMSG_QUEST_GIVER_QUEST_FAILED = 0x192, SMSG_QUEST_LOG_FULL = 0x195, SMSG_QUEST_UPDATE_FAILED = 0x196,
    SMSG_QUEST_UPDATE_COMPLETE = 0x198, SMSG_QUEST_UPDATE_ADD_KILL = 0x199, SMSG_QUEST_UPDATE_ADD_ITEM = 0x19a,
    SMSG_QUEST_CONFIRM_ACCEPT = 0x19c, SMSG_VENDOR_INVENTORY = 0x19f, SMSG_SELL_ITEM = 0x1a1,
    SMSG_BUY_FAILED = 0x1a5, SMSG_TRAINER_LIST = 0x1b1, SMSG_TRAINER_BUY_SUCCEEDED = 0x1b3,
    SMSG_LOG_XP_GAIN = 0x1d0, SMSG_CLEAR_COOLDOWN = 0x1de, SMSG_SPIRIT_HEALER_CONFIRM = 0x222,
    SMSG_SPELL_MISS_LOG = 0x24b, SMSG_SPELL_PERIODIC_AURA_LOG = 0x24e, SMSG_SPELL_NON_MELEE_DAMAGE_LOG = 0x250,
    SMSG_CORPSE_RECLAIM_DELAY = 0x269, SMSG_SPELL_FAILED_OTHER = 0x2a6, SMSG_INIT_WORLD_STATES = 0x2c2,
    SMSG_PLAY_SOUND = 0x2d2, SMSG_AURA_UPDATE_ALL = 0x495, SMSG_AURA_UPDATE = 0x496,
    SMSG_LOGIN_SETTIMESPEED = 0x042, SMSG_LEVELUP_INFO = 0x1d4, MSG_CORPSE_QUERY = 0x216, SMSG_STANDSTATE_UPDATE = 0x29d,
};
struct Reader {
    const std::vector<uint8_t> &p;
    size_t o = 0;
    bool more() const { return o < p.size(); }
    size_t left() const { return o < p.size() ? p.size() - o : 0; }
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
        return std::isfinite(v) ? v : 0.f;
    }
    uint64_t packed() {
        const uint8_t mask = u8();
        uint64_t guid = 0;
        for (unsigned i = 0; i < 8; ++i)
            if (mask & (1u << i))
                guid |= uint64_t(u8()) << (8 * i);
        return guid;
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
std::string n(uint64_t v) { return std::to_string(v); }
std::string si(int64_t v) { return std::to_string(v); }
std::string f(float v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", double(v));
    return text;
}
void put32(std::vector<uint8_t> &p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p.push_back(uint8_t(v >> (8 * i)));
}
void put64(std::vector<uint8_t> &p, uint64_t v) {
    put32(p, uint32_t(v));
    put32(p, uint32_t(v >> 32));
}
} // namespace

std::string guidHex(uint64_t guid) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(guid));
    return text;
}

void ServerEvents::corpseDistance(bool ghost, double distance) {
    const bool inRange = ghost && distance < 39.0;
    if (inRange != corpseInRange_) {
        corpseInRange_ = inRange;
        emit({inRange ? "CORPSE_IN_RANGE" : "CORPSE_OUT_OF_RANGE"});
    }
}

bool ServerEvents::handle(const WorldPacket &packet, const NameOf &nameOf, std::vector<WorldPacket> &out) {
    Reader r{packet.payload};
    auto name = [&](uint64_t guid) { return guid ? nameOf(guid) : std::string(); };
    try {
        switch (packet.opcode) {
        // ---- Auras.
        case SMSG_AURA_UPDATE_ALL:
        case SMSG_AURA_UPDATE: {
            const uint64_t guid = r.packed();
            if (packet.opcode == SMSG_AURA_UPDATE_ALL)
                emit({"__ANDROID_AURA_CLEAR", guidHex(guid)});
            while (r.more()) {
                const uint8_t slot = r.u8();
                const uint32_t spell = r.u32();
                if (!spell) {
                    emit({"__ANDROID_AURA", guidHex(guid), n(slot), "0"});
                    continue;
                }
                const uint8_t flags = r.u8(), level = r.u8(), stacks = r.u8();
                uint64_t caster = 0;
                if (!(flags & 0x08))
                    caster = r.packed();
                uint32_t maximum = 0, remaining = 0;
                if (flags & 0x20) {
                    maximum = r.u32();
                    remaining = r.u32();
                }
                emit({"__ANDROID_AURA", guidHex(guid), n(slot), n(spell), n(flags), n(level), n(stacks), n(maximum),
                      n(remaining), guidHex((flags & 0x08) ? guid : caster)});
            }
            return true;
        }
        // ---- Casting.
        case SMSG_SPELL_START: {
            r.packed();
            const uint64_t caster = r.packed();
            r.u8();
            const uint32_t spell = r.u32();
            r.u32();
            const uint32_t castTime = r.u32();
            emit({"__ANDROID_CAST_START", guidHex(caster), n(spell), n(castTime)});
            if (castTime > 0)
                act(caster, 52, castTime, true); // ReadySpellOmni while casting
            return true;
        }
        case SMSG_SPELL_GO: {
            r.packed();
            const uint64_t caster = r.packed();
            r.u8();
            const uint32_t spell = r.u32();
            emit({"__ANDROID_CAST_GO", guidHex(caster), n(spell), name(caster)});
            act(caster, 54); // SpellCastOmni
            return true;
        }
        case SMSG_SPELL_FAILURE:
        case SMSG_SPELL_FAILED_OTHER: {
            const uint64_t caster = r.packed();
            r.u8();
            const uint32_t spell = r.u32();
            const uint8_t reason = r.left() ? r.u8() : 0;
            emit({"__ANDROID_CAST_INTERRUPT", guidHex(caster), n(spell), n(reason)});
            act(caster, 0, 1); // back to the base animation
            return true;
        }
        case MSG_CHANNEL_START: {
            const uint64_t caster = r.packed();
            const uint32_t spell = r.u32(), duration = r.u32();
            emit({"__ANDROID_CHANNEL_START", guidHex(caster), n(spell), n(duration)});
            act(caster, 125, duration, true); // ChannelCastOmni (GL falls back to ReadySpellOmni)
            return true;
        }
        case MSG_CHANNEL_UPDATE: {
            const uint64_t caster = r.packed();
            const uint32_t remaining = r.u32();
            emit({"__ANDROID_CHANNEL_UPDATE", guidHex(caster), n(remaining)});
            if (!remaining)
                act(caster, 0, 1);
            return true;
        }
        // ---- Cooldowns.
        case SMSG_SPELL_COOLDOWN: {
            const uint64_t guid = r.u64();
            r.u8();
            if (guid != self_)
                return true;
            while (r.left() >= 8) {
                const uint32_t spell = r.u32(), ms = r.u32();
                emit({"__ANDROID_COOLDOWN", n(spell), n(ms)});
            }
            return true;
        }
        case SMSG_COOLDOWN_EVENT:
            emit({"__ANDROID_COOLDOWN_EVENT", n(r.u32())});
            return true;
        case SMSG_CLEAR_COOLDOWN:
            emit({"__ANDROID_COOLDOWN", n(r.u32()), "0"});
            return true;
        // ---- Melee and combat log.
        case SMSG_ATTACK_START: {
            const uint64_t attacker = r.u64(), victim = r.u64();
            emit({"__ANDROID_ATTACK_START", guidHex(attacker), guidHex(victim)});
            return true;
        }
        case SMSG_ATTACK_STOP: {
            const uint64_t attacker = r.packed(), victim = r.packed();
            emit({"__ANDROID_ATTACK_STOP", guidHex(attacker), guidHex(victim)});
            return true;
        }
        case SMSG_ATTACKER_STATE_UPDATE: {
            const uint32_t hit = r.u32();
            const uint64_t attacker = r.packed(), target = r.packed();
            const uint32_t damage = r.u32(), overkill = r.u32();
            const uint8_t parts = r.u8();
            uint32_t school = 1, absorbed = 0, resisted = 0;
            for (uint8_t i = 0; i < parts && i < 8; ++i) {
                const uint32_t mask = r.u32();
                r.f32();
                r.u32();
                if (!i)
                    school = mask;
            }
            if (hit & (0x20 | 0x40))
                for (uint8_t i = 0; i < parts && i < 8; ++i)
                    absorbed += r.u32();
            if (hit & (0x80 | 0x100))
                for (uint8_t i = 0; i < parts && i < 8; ++i)
                    resisted += r.u32();
            const uint8_t victimState = r.left() ? r.u8() : 1;
            act(attacker, 17); // Attack1H (falls back to AttackUnarmed on the GL side)
            if (damage && victimState == 1)
                act(target, 9); // CombatWound
            else if (victimState == 2)
                act(target, 30); // Dodge
            else if (victimState == 3)
                act(target, 21); // Parry1H
            emit({"__ANDROID_SWING", guidHex(attacker), name(attacker), guidHex(target), name(target), n(damage),
                  n(overkill), n(school), n(absorbed), n(resisted), n(hit), n(victimState)});
            return true;
        }
        case SMSG_SPELL_NON_MELEE_DAMAGE_LOG: {
            const uint64_t target = r.packed(), caster = r.packed();
            const uint32_t spell = r.u32(), damage = r.u32(), overkill = r.u32();
            const uint8_t school = r.u8();
            const uint32_t absorbed = r.u32(), resisted = r.u32();
            r.u8();
            r.u8();
            const uint32_t blocked = r.u32(), flags = r.u32();
            emit({"__ANDROID_SPELL_DAMAGE", guidHex(caster), name(caster), guidHex(target), name(target), n(spell),
                  n(damage), n(overkill), n(school), n(absorbed), n(resisted), n(blocked), n(flags & 2 ? 1 : 0)});
            return true;
        }
        case SMSG_SPELL_HEAL_LOG: {
            const uint64_t target = r.packed(), caster = r.packed();
            const uint32_t spell = r.u32(), heal = r.u32(), overheal = r.u32(), absorbed = r.u32();
            const uint8_t crit = r.u8();
            emit({"__ANDROID_SPELL_HEAL", guidHex(caster), name(caster), guidHex(target), name(target), n(spell),
                  n(heal), n(overheal), n(absorbed), n(crit)});
            return true;
        }
        case SMSG_SPELL_PERIODIC_AURA_LOG: {
            const uint64_t target = r.packed(), caster = r.packed();
            const uint32_t spell = r.u32(), count = r.u32();
            for (uint32_t i = 0; i < count && i < 4; ++i) {
                const uint32_t aura = r.u32();
                if (aura == 3 || aura == 89) {
                    const uint32_t damage = r.u32(), overkill = r.u32(), school = r.u32(), absorbed = r.u32(),
                                   resisted = r.u32();
                    const uint8_t crit = r.u8();
                    emit({"__ANDROID_PERIODIC_DAMAGE", guidHex(caster), name(caster), guidHex(target), name(target),
                          n(spell), n(damage), n(overkill), n(school), n(absorbed), n(resisted), n(crit)});
                } else if (aura == 8 || aura == 20) {
                    const uint32_t heal = r.u32(), overheal = r.u32(), absorbed = r.u32();
                    const uint8_t crit = r.u8();
                    emit({"__ANDROID_PERIODIC_HEAL", guidHex(caster), name(caster), guidHex(target), name(target),
                          n(spell), n(heal), n(overheal), n(absorbed), n(crit)});
                } else if (aura == 24 || aura == 21) {
                    const uint32_t power = r.u32(), amount = r.u32();
                    emit({"__ANDROID_PERIODIC_ENERGIZE", guidHex(caster), name(caster), guidHex(target), name(target),
                          n(spell), n(amount), n(power)});
                } else
                    break;
            }
            return true;
        }
        case SMSG_SPELL_MISS_LOG: {
            const uint32_t spell = r.u32();
            const uint64_t caster = r.u64();
            r.u8();
            const uint32_t count = r.u32();
            for (uint32_t i = 0; i < count && i < 32; ++i) {
                const uint64_t target = r.u64();
                const uint8_t miss = r.u8();
                emit({"__ANDROID_SPELL_MISS", guidHex(caster), name(caster), guidHex(target), name(target), n(spell),
                      n(miss)});
            }
            return true;
        }
        case SMSG_LOG_XP_GAIN: {
            const uint64_t victim = r.u64();
            const uint32_t xp = r.u32();
            const uint8_t type = r.u8();
            emit({"__ANDROID_XP_GAIN", guidHex(victim), name(victim), n(xp), n(type)});
            return true;
        }
        case SMSG_LEVELUP_INFO: {
            const uint32_t level = r.u32(), hp = r.u32();
            Event event{"__ANDROID_LEVEL_UP", n(level), n(hp)};
            for (int i = 0; i < 6 && r.left() >= 4; ++i)
                event.push_back(n(r.u32())); // powers
            for (int i = 0; i < 5 && r.left() >= 4; ++i)
                event.push_back(n(r.u32())); // stats
            emit(std::move(event));
            return true;
        }
        // ---- Loot.
        case SMSG_LOOT_RESPONSE: {
            const uint64_t guid = r.u64();
            const uint8_t type = r.u8();
            if (r.left() < 5) {
                emit({"__ANDROID_LOOT_ERROR", guidHex(guid), n(r.left() ? r.u8() : 0)});
                return true;
            }
            const uint32_t gold = r.u32();
            const uint8_t count = r.u8();
            Event event{"__ANDROID_LOOT", guidHex(guid), n(type), n(gold), n(count)};
            for (uint8_t i = 0; i < count; ++i) {
                const uint8_t slot = r.u8();
                const uint32_t item = r.u32(), itemCount = r.u32();
                r.u32(); // display
                r.u32(); // random suffix
                r.u32(); // random property
                const uint8_t slotType = r.u8();
                event.push_back(n(slot));
                event.push_back(n(item));
                event.push_back(n(itemCount));
                event.push_back(n(slotType));
            }
            emit(std::move(event));
            return true;
        }
        case SMSG_LOOT_REMOVED:
            emit({"__ANDROID_LOOT_REMOVED", n(r.u8())});
            return true;
        case SMSG_LOOT_RELEASE:
            emit({"LOOT_CLOSED"});
            return true;
        case SMSG_LOOT_MONEY_NOTIFY:
            emit({"__ANDROID_LOOT_MONEY", n(r.u32())});
            return true;
        case SMSG_LOOT_CLEAR_MONEY:
            emit({"__ANDROID_LOOT_CLEAR_MONEY"});
            return true;
        case SMSG_ITEM_PUSH_RESULT: {
            const uint64_t player = r.u64();
            const uint32_t received = r.u32(), created = r.u32(), chat = r.u32();
            r.u8();
            r.u32();
            const uint32_t item = r.u32();
            r.u32();
            r.u32();
            const uint32_t count = r.u32();
            if (chat)
                emit({"__ANDROID_ITEM_PUSH", guidHex(player), name(player), n(item), n(count), n(received), n(created)});
            return true;
        }
        // ---- NPC dialogs.
        case SMSG_GOSSIP_MESSAGE: {
            const uint64_t guid = r.u64();
            const uint32_t menu = r.u32(), text = r.u32(), options = r.u32();
            Event event{"__ANDROID_GOSSIP", guidHex(guid), n(menu), n(text), n(options)};
            for (uint32_t i = 0; i < options && i < 64; ++i) {
                event.push_back(n(r.u32())); // index
                event.push_back(n(r.u8()));  // icon
                event.push_back(n(r.u8()));  // coded
                r.u32();                     // box money
                event.push_back(r.str());    // text
                r.str();                     // box text
            }
            const uint32_t quests = r.left() >= 4 ? r.u32() : 0;
            event.push_back(n(quests));
            for (uint32_t i = 0; i < quests && i < 64; ++i) {
                event.push_back(n(r.u32())); // quest
                event.push_back(n(r.u32())); // icon
                event.push_back(si(r.i32())); // level
                r.u32();                      // flags
                r.u8();                       // repeatable
                event.push_back(r.str());
            }
            emit(std::move(event));
            WorldPacket query{CMSG_QUERY_NPC_TEXT, {}};
            put32(query.payload, text);
            put64(query.payload, guid);
            out.push_back(std::move(query));
            return true;
        }
        case SMSG_GOSSIP_COMPLETE:
            emit({"GOSSIP_CLOSED"});
            return true;
        case SMSG_QUERY_NPC_TEXT_RESPONSE: {
            const uint32_t id = r.u32();
            std::string text;
            for (int i = 0; i < 8 && r.left() >= 4; ++i) {
                r.f32();
                const std::string a = r.str(), b = r.str();
                if (text.empty())
                    text = !a.empty() ? a : b;
                r.u32();
                for (int k = 0; k < 6; ++k)
                    r.u32();
            }
            emit({"__ANDROID_NPC_TEXT", n(id), text});
            return true;
        }
        case SMSG_QUEST_GIVER_QUEST_LIST_MESSAGE: {
            const uint64_t guid = r.u64();
            const std::string greeting = r.str();
            r.u32();
            r.u32();
            const uint8_t count = r.u8();
            Event event{"__ANDROID_QUEST_LIST", guidHex(guid), greeting, n(count)};
            for (uint8_t i = 0; i < count; ++i) {
                event.push_back(n(r.u32()));
                event.push_back(n(r.u32()));
                event.push_back(si(r.i32()));
                r.u32();
                r.u8();
                event.push_back(r.str());
            }
            emit(std::move(event));
            return true;
        }
        case SMSG_QUEST_GIVER_QUEST_DETAILS: {
            const uint64_t guid = r.u64();
            r.u64();
            const uint32_t quest = r.u32();
            const std::string title = r.str(), details = r.str(), objectives = r.str();
            r.u8();
            const uint32_t flags = r.u32();
            r.u32();
            r.u8();
            Event event{"__ANDROID_QUEST_DETAIL", guidHex(guid), n(quest), title, details, objectives};
            uint32_t money = 0;
            Event choices, rewards;
            if (flags & 0x200) {
                r.u32();
                r.u32();
                r.u32();
            } else {
                const uint32_t choiceCount = r.u32();
                for (uint32_t i = 0; i < choiceCount && i < 6; ++i) {
                    choices.push_back(n(r.u32()));
                    choices.push_back(n(r.u32()));
                    r.u32();
                }
                const uint32_t rewardCount = r.u32();
                for (uint32_t i = 0; i < rewardCount && i < 4; ++i) {
                    rewards.push_back(n(r.u32()));
                    rewards.push_back(n(r.u32()));
                    r.u32();
                }
                money = r.u32();
            }
            event.push_back(n(money));
            event.push_back(n(choices.size() / 2));
            event.insert(event.end(), choices.begin(), choices.end());
            event.push_back(n(rewards.size() / 2));
            event.insert(event.end(), rewards.begin(), rewards.end());
            emit(std::move(event));
            return true;
        }
        case SMSG_QUEST_GIVER_REQUEST_ITEMS: {
            const uint64_t guid = r.u64();
            const uint32_t quest = r.u32();
            const std::string title = r.str(), text = r.str();
            r.u32();
            r.u32();
            r.u32();
            r.u32();
            r.u32();
            const uint32_t money = r.u32(), items = r.u32();
            Event event{"__ANDROID_QUEST_PROGRESS", guidHex(guid), n(quest), title, text, n(money), n(items)};
            for (uint32_t i = 0; i < items && i < 6; ++i) {
                event.push_back(n(r.u32()));
                event.push_back(n(r.u32()));
                r.u32();
            }
            const uint32_t completable = r.left() >= 4 ? r.u32() : 0;
            event.push_back(n(completable));
            emit(std::move(event));
            return true;
        }
        case SMSG_QUEST_GIVER_OFFER_REWARD_MESSAGE: {
            const uint64_t guid = r.u64();
            const uint32_t quest = r.u32();
            const std::string title = r.str(), text = r.str();
            r.u8();
            r.u32();
            r.u32();
            const uint32_t emotes = r.u32();
            for (uint32_t i = 0; i < emotes && i < 8; ++i) {
                r.u32();
                r.u32();
            }
            Event event{"__ANDROID_QUEST_COMPLETE", guidHex(guid), n(quest), title, text};
            const uint32_t choiceCount = r.u32();
            Event choices, rewards;
            for (uint32_t i = 0; i < choiceCount && i < 6; ++i) {
                choices.push_back(n(r.u32()));
                choices.push_back(n(r.u32()));
                r.u32();
            }
            const uint32_t rewardCount = r.u32();
            for (uint32_t i = 0; i < rewardCount && i < 4; ++i) {
                rewards.push_back(n(r.u32()));
                rewards.push_back(n(r.u32()));
                r.u32();
            }
            const uint32_t money = r.left() >= 4 ? r.u32() : 0;
            const uint32_t xp = r.left() >= 4 ? r.u32() : 0;
            event.push_back(n(money));
            event.push_back(n(xp));
            event.push_back(n(choices.size() / 2));
            event.insert(event.end(), choices.begin(), choices.end());
            event.push_back(n(rewards.size() / 2));
            event.insert(event.end(), rewards.begin(), rewards.end());
            emit(std::move(event));
            return true;
        }
        case SMSG_QUEST_GIVER_QUEST_COMPLETE: {
            const uint32_t quest = r.u32(), xp = r.u32(), money = r.u32();
            emit({"__ANDROID_QUEST_DONE", n(quest), n(xp), n(money)});
            return true;
        }
        case SMSG_QUEST_GIVER_INVALID_QUEST:
            emit({"__ANDROID_QUEST_INVALID", n(r.u32())});
            return true;
        case SMSG_QUEST_GIVER_QUEST_FAILED:
        case SMSG_QUEST_UPDATE_FAILED:
            emit({"__ANDROID_QUEST_FAILED", n(r.u32())});
            return true;
        case SMSG_QUEST_LOG_FULL:
            emit({"__ANDROID_QUEST_LOG_FULL"});
            return true;
        case SMSG_QUEST_UPDATE_COMPLETE:
            emit({"__ANDROID_QUEST_OBJECTIVES_COMPLETE", n(r.u32())});
            return true;
        case SMSG_QUEST_UPDATE_ADD_KILL: {
            const uint32_t quest = r.u32(), entry = r.u32(), count = r.u32(), required = r.u32();
            emit({"__ANDROID_QUEST_KILL", n(quest), n(entry), n(count), n(required)});
            return true;
        }
        case SMSG_QUEST_UPDATE_ADD_ITEM:
            return true; // the item arrives in the bags; QUEST_LOG_UPDATE follows from the fields
        case SMSG_QUEST_CONFIRM_ACCEPT: {
            const uint32_t quest = r.u32();
            const std::string title = r.str();
            const uint64_t guid = r.u64();
            emit({"__ANDROID_QUEST_ACCEPT_CONFIRM", n(quest), title, guidHex(guid), name(guid)});
            return true;
        }
        // ---- Vendor and trainer.
        case SMSG_VENDOR_INVENTORY: {
            const uint64_t vendor = r.u64();
            const uint8_t count = r.u8();
            Event event{"__ANDROID_VENDOR", guidHex(vendor), n(count)};
            for (uint8_t i = 0; i < count; ++i) {
                event.push_back(n(r.u32()));  // slot
                event.push_back(n(r.u32()));  // item
                r.u32();                      // display
                event.push_back(si(r.i32())); // available (-1 unlimited)
                event.push_back(n(r.u32()));  // price
                event.push_back(n(r.u32()));  // durability
                event.push_back(n(r.u32()));  // stack (buy count)
                event.push_back(n(r.u32()));  // extended cost
            }
            emit(std::move(event));
            return true;
        }
        case SMSG_SELL_ITEM: {
            r.u64();
            r.u64();
            const uint8_t error = r.u8();
            if (error)
                emit({"__ANDROID_VENDOR_ERROR", "sell", n(error)});
            return true;
        }
        case SMSG_BUY_FAILED: {
            r.u64();
            r.u32();
            emit({"__ANDROID_VENDOR_ERROR", "buy", n(r.u8())});
            return true;
        }
        case SMSG_TRAINER_LIST: {
            const uint64_t guid = r.u64();
            const uint32_t type = r.u32(), count = r.u32();
            Event event{"__ANDROID_TRAINER", guidHex(guid), n(type), n(count)};
            for (uint32_t i = 0; i < count && i < 512; ++i) {
                event.push_back(n(r.u32())); // spell
                event.push_back(n(r.u8()));  // state 0 available, 1 unavailable, 2 used
                event.push_back(n(r.u32())); // cost
                r.u32();
                r.u32();
                event.push_back(n(r.u8()));  // required level
                event.push_back(n(r.u32())); // required skill
                event.push_back(n(r.u32())); // required skill value
                r.u32();
                r.u32();
                r.u32();
            }
            event.push_back(r.left() ? r.str() : std::string());
            emit(std::move(event));
            return true;
        }
        case SMSG_TRAINER_BUY_SUCCEEDED: {
            const uint64_t guid = r.u64();
            emit({"__ANDROID_TRAINER_BOUGHT", guidHex(guid), n(r.u32())});
            return true;
        }
        // ---- Death.
        case SMSG_RESURRECT_REQUEST: {
            const uint64_t guid = r.u64();
            r.u32();
            const std::string who = r.str();
            emit({"__ANDROID_RESURRECT_REQUEST", guidHex(guid), who.empty() ? name(guid) : who});
            return true;
        }
        case SMSG_SPIRIT_HEALER_CONFIRM:
            emit({"__ANDROID_SPIRIT_HEALER", guidHex(r.u64())});
            return true;
        case MSG_CORPSE_QUERY: {
            if (!r.u8()) {
                emit({"__ANDROID_CORPSE", "0"});
                return true;
            }
            const uint32_t map = r.u32();
            const float x = r.f32(), y = r.f32(), z = r.f32();
            emit({"__ANDROID_CORPSE", "1", n(map), f(x), f(y), f(z)});
            return true;
        }
        case SMSG_LOGIN_SETTIMESPEED: {
            // Packed game time: minute (6 bits), hour (5), weekday (3), day, month, year.
            const uint32_t packed = r.u32();
            const uint32_t minute = packed & 0x3f, hour = (packed >> 6) & 0x1f;
            gameHalfMinutes_ = (hour * 60 + minute) * 2;
            gameTimeSerial_++;
            emit({"__ANDROID_GAME_TIME", n(hour), n(minute)});
            return true;
        }
        case SMSG_STANDSTATE_UPDATE:
            emit({"__ANDROID_STAND_STATE", n(r.u8())});
            return true;
        case SMSG_CORPSE_RECLAIM_DELAY:
            emit({"__ANDROID_CORPSE_DELAY", n(r.u32())});
            return true;
        // ---- Social.
        case SMSG_CONTACT_LIST: {
            r.u32();
            const uint32_t count = r.u32();
            Event event{"__ANDROID_CONTACTS", n(count)};
            for (uint32_t i = 0; i < count && i < 200; ++i) {
                const uint64_t guid = r.u64();
                const uint32_t flags = r.u32();
                const std::string note = r.str();
                uint32_t status = 0, area = 0, level = 0, cls = 0;
                if (flags & 1) {
                    status = r.u8();
                    if (status) {
                        area = r.u32();
                        level = r.u32();
                        cls = r.u32();
                    }
                }
                event.insert(event.end(), {guidHex(guid), name(guid), n(flags), note, n(status), n(area), n(level),
                                           n(cls)});
            }
            emit(std::move(event));
            return true;
        }
        case SMSG_FRIEND_STATUS: {
            const uint8_t result = r.u8();
            const uint64_t guid = r.u64();
            emit({"__ANDROID_FRIEND_STATUS", n(result), guidHex(guid), name(guid)});
            return true;
        }
        case SMSG_WHO: {
            const uint32_t shown = r.u32(), total = r.u32();
            Event event{"__ANDROID_WHO", n(shown), n(total)};
            for (uint32_t i = 0; i < shown && i < 50; ++i) {
                event.push_back(r.str());
                event.push_back(r.str());
                event.push_back(n(r.u32()));
                event.push_back(n(r.u32()));
                event.push_back(n(r.u32()));
                r.u8();
                event.push_back(n(r.u32()));
            }
            emit(std::move(event));
            return true;
        }
        case SMSG_QUERY_GUILD_INFO_RESPONSE: {
            const uint32_t id = r.u32();
            Event event{"__ANDROID_GUILD_INFO", n(id), r.str()};
            for (int i = 0; i < 10; ++i)
                event.push_back(r.str());
            emit(std::move(event));
            return true;
        }
        case SMSG_GUILD_ROSTER: {
            const uint32_t count = r.u32();
            const std::string motd = r.str(), info = r.str();
            const uint32_t ranks = r.u32();
            for (uint32_t i = 0; i < ranks && i < 10; ++i) {
                r.u32();
                r.u32();
                for (int k = 0; k < 12; ++k)
                    r.u32();
            }
            Event event{"__ANDROID_GUILD_ROSTER", motd, info, n(count)};
            for (uint32_t i = 0; i < count && i < 1000; ++i) {
                const uint64_t guid = r.u64();
                const uint8_t online = r.u8();
                const std::string who = r.str();
                const uint32_t rank = r.u32();
                const uint8_t level = r.u8(), cls = r.u8();
                r.u8();
                const uint32_t zone = r.u32();
                if (!online)
                    r.f32();
                const std::string note = r.str(), officer = r.str();
                event.insert(event.end(), {guidHex(guid), who, n(online), n(rank), n(level), n(cls), n(zone), note});
            }
            emit(std::move(event));
            return true;
        }
        case SMSG_GUILD_EVENT:
            emit({"__ANDROID_GUILD_EVENT"});
            return true;
        // ---- World.
        case SMSG_INIT_WORLD_STATES: {
            const uint32_t map = r.u32(), zone = r.u32(), area = r.u32();
            emit({"__ANDROID_ZONE", n(map), n(zone), n(area)});
            return true;
        }
        case SMSG_EMOTE: {
            const uint32_t anim = r.u32();
            const uint64_t who = r.u64();
            emit({"__ANDROID_EMOTE", guidHex(who), n(anim)});
            // Dance, sit/sleep/kneel style emotes loop until the unit moves.
            const bool state = anim == 69 || anim == 71 || anim == 72 || anim == 75 || anim == 97;
            act(who, uint16_t(anim), state ? 600000 : 0, state);
            return true;
        }
        case SMSG_PLAY_SOUND:
            emit({"__ANDROID_PLAY_SOUND", n(r.u32())});
            return true;
        case SMSG_INVENTORY_CHANGE_FAILURE: {
            const uint8_t error = r.u8();
            if (error)
                emit({"__ANDROID_INVENTORY_ERROR", n(error)});
            return true;
        }
        default:
            return false;
        }
    } catch (const std::exception &) {
        return true; // malformed or another layout: dropped
    }
}
} // namespace wow::android
