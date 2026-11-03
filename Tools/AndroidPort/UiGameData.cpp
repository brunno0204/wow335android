// Client database access for the UI game layer (ui/*.lua): DBFilesClient tables by field
// name, with the column layouts of the decompiled record readers (UiDbcLayouts.inc).
#include "UiGameData.h"
#include "UiRuntime.h"
#include "ClientDb.h"
#include "M2File.h"
#include "WorldLink.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

namespace wow::android {
namespace {
#include "UiDbcLayouts.inc"

struct Table {
    DbcTable data;
    std::unordered_map<uint32_t, uint32_t> rows; // id (column 0) -> row
    std::map<std::string, const UiDbcField *> fields;
    bool loaded = false;
};
std::map<std::string, std::unique_ptr<Table>> tables;

Table *table(const char *name) {
    if (!name)
        return nullptr;
    auto &slot = tables[name];
    if (slot)
        return slot->loaded ? slot.get() : nullptr;
    slot = std::make_unique<Table>();
    for (const auto &field : kUiDbcFields)
        if (!std::strcmp(field.table, name))
            slot->fields[field.field] = &field;
    if (slot->fields.empty())
        return nullptr;
    try {
        slot->data = DbcTable(m2::readFile(std::string("DBFilesClient\\") + name + ".dbc", 96 * 1024 * 1024), 0, 0,
                              name);
        for (uint32_t r = 0; r < slot->data.rows(); ++r)
            slot->rows.emplace(slot->data.u32(r, 0), r);
        slot->loaded = true;
    } catch (const std::exception &) {
        slot->loaded = false;
    }
    return slot->loaded ? slot.get() : nullptr;
}
// Pushes the named fields of one row (Lua arguments from index first).
int pushFields(lua_State *L, Table &t, uint32_t row, int first) {
    const int top = lua_gettop(L);
    int pushed = 0;
    for (int i = first; i <= top; ++i) {
        const char *name = lua_tostring(L, i);
        const auto found = name ? t.fields.find(name) : t.fields.end();
        if (found == t.fields.end()) {
            lua_pushnil(L);
        } else {
            const UiDbcField &f = *found->second;
            try {
                switch (f.kind) {
                case 'f':
                    lua_pushnumber(L, t.data.f32(row, f.column));
                    break;
                case 's': {
                    const auto text = t.data.str(row, f.column);
                    lua_pushlstring(L, text.data(), text.size());
                    break;
                }
                case 'l': {
                    const auto text = t.data.loc(row, f.column);
                    lua_pushlstring(L, text.data(), text.size());
                    break;
                }
                default:
                    lua_pushnumber(L, t.data.i32(row, f.column));
                }
            } catch (const std::exception &) {
                lua_pushnil(L);
            }
        }
        ++pushed;
    }
    return pushed;
}
// __AndroidDbc(table, id, field, ...) -> values, or nothing when the row does not exist.
int lDbc(lua_State *L) {
    Table *t = table(lua_tostring(L, 1));
    if (!t || !lua_isnumber(L, 2))
        return 0;
    const auto found = t->rows.find(uint32_t(lua_tonumber(L, 2)));
    if (found == t->rows.end())
        return 0;
    return pushFields(L, *t, found->second, 3);
}
// __AndroidDbcRow(table, rowIndex (1-based), field, ...) -> values, for scanning a table.
int lDbcRow(lua_State *L) {
    Table *t = table(lua_tostring(L, 1));
    if (!t || !lua_isnumber(L, 2))
        return 0;
    const auto index = int64_t(lua_tonumber(L, 2)) - 1;
    if (index < 0 || index >= int64_t(t->data.rows()))
        return 0;
    return pushFields(L, *t, uint32_t(index), 3);
}
// __AndroidDbcCount(table) -> number of rows (0 when missing).
int lDbcCount(lua_State *L) {
    Table *t = table(lua_tostring(L, 1));
    lua_pushnumber(L, t ? t->data.rows() : 0);
    return 1;
}
// ---- Player data snapshots (network thread -> UiRuntime::sync -> here).
std::shared_ptr<const PlayerFieldsView> fieldsView;
std::shared_ptr<const PlayerDataView> dataView;

std::string guidText(uint64_t guid) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(guid));
    return text;
}
uint64_t guidArg(lua_State *L, int index) {
    const char *text = lua_tostring(L, index);
    if (!text || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
        return 0;
    return std::strtoull(text + 2, nullptr, 16);
}
uint32_t field(const std::vector<uint32_t> &fields, uint32_t index) {
    return index < fields.size() ? fields[index] : 0;
}
uint64_t guidField(const std::vector<uint32_t> &fields, uint32_t index) {
    return uint64_t(field(fields, index)) | uint64_t(field(fields, index + 1)) << 32;
}
// __AndroidPlayerField(index[, "guid"|"float"]) -> raw player update field.
int lPlayerField(lua_State *L) {
    if (!fieldsView || !lua_isnumber(L, 1))
        return 0;
    const auto index = uint32_t(lua_tonumber(L, 1));
    const char *kind = lua_tostring(L, 2);
    if (kind && !std::strcmp(kind, "guid")) {
        const uint64_t guid = guidField(fieldsView->fields, index);
        if (!guid)
            return 0;
        const auto text = guidText(guid);
        lua_pushstring(L, text.c_str());
        return 1;
    }
    if (kind && !std::strcmp(kind, "float")) {
        const uint32_t bits = field(fieldsView->fields, index);
        float value;
        std::memcpy(&value, &bits, 4);
        lua_pushnumber(L, value == value ? value : 0);
        return 1;
    }
    lua_pushnumber(L, field(fieldsView->fields, index));
    return 1;
}
// __AndroidItemObject(guid) -> entry, stack count, durability, max durability, container slots, flags.
int lItemObject(lua_State *L) {
    const uint64_t guid = guidArg(L, 1);
    if (!fieldsView || !guid)
        return 0;
    const auto found = fieldsView->items.find(guid);
    if (found == fieldsView->items.end())
        return 0;
    const auto &f = found->second;
    lua_pushnumber(L, field(f, 0x03));  // OBJECT_FIELD_ENTRY
    lua_pushnumber(L, field(f, 0x0e));  // ITEM_FIELD_STACK_COUNT
    lua_pushnumber(L, field(f, 0x3c));  // ITEM_FIELD_DURABILITY
    lua_pushnumber(L, field(f, 0x3d));  // ITEM_FIELD_MAXDURABILITY
    lua_pushnumber(L, f.size() > 0x40 ? field(f, 0x40) : 0); // CONTAINER_FIELD_NUM_SLOTS
    lua_pushnumber(L, field(f, 0x15));  // ITEM_FIELD_FLAGS
    return 6;
}
// __AndroidContainerSlot(containerGuid, slot (1-based)) -> item GUID or nil.
int lContainerSlot(lua_State *L) {
    const uint64_t guid = guidArg(L, 1);
    if (!fieldsView || !guid || !lua_isnumber(L, 2))
        return 0;
    const auto found = fieldsView->items.find(guid);
    const int slot = int(lua_tonumber(L, 2)) - 1;
    if (found == fieldsView->items.end() || slot < 0 || slot >= 36)
        return 0;
    const uint64_t item = guidField(found->second, uint32_t(0x42 + slot * 2));
    if (!item)
        return 0;
    const auto text = guidText(item);
    lua_pushstring(L, text.c_str());
    return 1;
}
const ItemTemplate *itemTemplate(uint32_t entry) {
    if (!dataView)
        return nullptr;
    const auto found = dataView->itemTemplates.find(entry);
    return found != dataView->itemTemplates.end() ? &found->second : nullptr;
}
// __AndroidItemTemplate(entry) -> name, quality, itemLevel, requiredLevel, class, subclass,
//   maxStack, inventoryType, displayId, sellPrice, armor, delay, damageMin, damageMax, bonding,
//   maxDurability, description, containerSlots, buyPrice, damageType; nil (and a query) if unknown.
int lItemTemplate(lua_State *L) {
    if (!lua_isnumber(L, 1))
        return 0;
    const auto entry = uint32_t(lua_tonumber(L, 1));
    const ItemTemplate *t = itemTemplate(entry);
    if (!t) {
        if (entry) {
            std::lock_guard<std::mutex> link(worldLinkMutex());
            worldLink().wantItems.push_back(entry);
        }
        return 0;
    }
    lua_pushstring(L, t->name.c_str());
    lua_pushnumber(L, t->quality);
    lua_pushnumber(L, t->itemLevel);
    lua_pushnumber(L, t->requiredLevel);
    lua_pushnumber(L, t->itemClass);
    lua_pushnumber(L, t->subClass);
    lua_pushnumber(L, t->maxStack);
    lua_pushnumber(L, t->inventoryType);
    lua_pushnumber(L, t->display);
    lua_pushnumber(L, t->sellPrice);
    lua_pushnumber(L, t->armor);
    lua_pushnumber(L, t->delay);
    lua_pushnumber(L, t->damageMin);
    lua_pushnumber(L, t->damageMax);
    lua_pushnumber(L, t->bonding);
    lua_pushnumber(L, t->maxDurability);
    lua_pushstring(L, t->description.c_str());
    lua_pushnumber(L, t->containerSlots);
    lua_pushnumber(L, t->buyPrice);
    lua_pushnumber(L, t->damageType);
    return 20;
}
// __AndroidItemStats(entry) -> type1, value1, type2, value2, ...
int lItemStats(lua_State *L) {
    const ItemTemplate *t = lua_isnumber(L, 1) ? itemTemplate(uint32_t(lua_tonumber(L, 1))) : nullptr;
    if (!t)
        return 0;
    lua_checkstack(L, int(t->stats.size() * 2 + 10));
    for (const auto &[type, value] : t->stats) {
        lua_pushnumber(L, type);
        lua_pushnumber(L, value);
    }
    return int(t->stats.size() * 2);
}
// __AndroidItemSpells(entry) -> spell1, trigger1, ... (5 pairs).
int lItemSpells(lua_State *L) {
    const ItemTemplate *t = lua_isnumber(L, 1) ? itemTemplate(uint32_t(lua_tonumber(L, 1))) : nullptr;
    if (!t)
        return 0;
    for (size_t i = 0; i < 5; ++i) {
        lua_pushnumber(L, t->spells[i]);
        lua_pushnumber(L, t->spellTriggers[i]);
    }
    return 10;
}
// __AndroidSpells() -> known spell IDs.
int lSpells(lua_State *L) {
    if (!dataView)
        return 0;
    lua_checkstack(L, int(dataView->spells.size()) + 10);
    for (uint32_t spell : dataView->spells)
        lua_pushnumber(L, spell);
    return int(dataView->spells.size());
}
// __AndroidAction(slot 1..144) -> action id, type (0 spell, 64 macro, 128 item) or nil.
int lAction(lua_State *L) {
    if (!dataView || !lua_isnumber(L, 1))
        return 0;
    const int slot = int(lua_tonumber(L, 1)) - 1;
    if (slot < 0 || slot >= 144 || !dataView->actions[size_t(slot)])
        return 0;
    const uint32_t value = dataView->actions[size_t(slot)];
    lua_pushnumber(L, value & 0xffffff);
    lua_pushnumber(L, value >> 24);
    return 2;
}
// __AndroidSetAction(slot, value): local copy until the server echoes (CMSG_SET_ACTION_BUTTON sent by Lua).
int lSetAction(lua_State *L) {
    if (!dataView || !lua_isnumber(L, 1))
        return 0;
    const int slot = int(lua_tonumber(L, 1)) - 1;
    if (slot < 0 || slot >= 144)
        return 0;
    auto copy = std::make_shared<PlayerDataView>(*dataView);
    copy->actions[size_t(slot)] = uint32_t(lua_tonumber(L, 2));
    dataView = std::move(copy);
    return 0;
}
// __AndroidQuest(id) -> title, level, type, zoneOrSort, flags, objectives, details, suggestedPlayers,
//   endText, money; then 4 x (creature, count, text) and 6 x (item, count).
int lQuest(lua_State *L) {
    if (!dataView || !lua_isnumber(L, 1))
        return 0;
    const auto found = dataView->quests.find(uint32_t(lua_tonumber(L, 1)));
    if (found == dataView->quests.end())
        return 0;
    const auto &q = found->second;
    lua_checkstack(L, 40);
    lua_pushstring(L, q.title.c_str());
    lua_pushnumber(L, q.level);
    lua_pushnumber(L, q.type);
    lua_pushnumber(L, q.zoneOrSort);
    lua_pushnumber(L, q.flags);
    lua_pushstring(L, q.objectives.c_str());
    lua_pushstring(L, q.details.c_str());
    lua_pushnumber(L, q.suggestedPlayers);
    lua_pushstring(L, q.endText.c_str());
    lua_pushnumber(L, q.money);
    for (size_t i = 0; i < 4; ++i) {
        lua_pushnumber(L, q.creature[i]);
        lua_pushnumber(L, q.creatureCount[i]);
        lua_pushstring(L, q.objectiveText[i].c_str());
    }
    for (size_t i = 0; i < 6; ++i) {
        lua_pushnumber(L, q.item[i]);
        lua_pushnumber(L, q.itemCount[i]);
    }
    return 10 + 12 + 12;
}
// __AndroidObjectiveName(target) -> creature (target > 0) or game object (target < 0) name.
int lObjectiveName(lua_State *L) {
    if (!dataView || !lua_isnumber(L, 1))
        return 0;
    const auto target = int64_t(lua_tonumber(L, 1));
    const auto &names = target > 0 ? dataView->creatureNames : dataView->gameObjectNames;
    const auto found = names.find(uint32_t(target > 0 ? target : -target));
    if (found == names.end())
        return 0;
    lua_pushstring(L, found->second.c_str());
    return 1;
}
// __AndroidParty() -> leaderGuid, groupType, lootMethod, lootThreshold, then per member:
//   name, guid, online, subGroup, flags.
int lParty(lua_State *L) {
    if (!dataView)
        return 0;
    lua_checkstack(L, int(dataView->party.size() * 5 + 10));
    const auto leader = guidText(dataView->leader);
    lua_pushstring(L, leader.c_str());
    lua_pushnumber(L, dataView->groupType);
    lua_pushnumber(L, dataView->lootMethod);
    lua_pushnumber(L, dataView->lootThreshold);
    for (const auto &m : dataView->party) {
        lua_pushstring(L, m.name.c_str());
        const auto guid = guidText(m.guid);
        lua_pushstring(L, guid.c_str());
        lua_pushnumber(L, m.online);
        lua_pushnumber(L, m.subGroup);
        lua_pushnumber(L, m.flags);
    }
    return 4 + int(dataView->party.size() * 5);
}
// __AndroidSend(opcode, format, ...): builds a client packet. Format letters: b u8, h u16,
// i u32 (signed ok), f float, l u64 from a "0x..." GUID, p packed GUID, s C string.
int lSend(lua_State *L) {
    if (!lua_isnumber(L, 1))
        return 0;
    WorldPacket packet{uint32_t(lua_tonumber(L, 1)), {}};
    const char *format = lua_tostring(L, 2);
    auto &out = packet.payload;
    auto put = [&](uint64_t value, int bytes) {
        for (int i = 0; i < bytes; ++i)
            out.push_back(uint8_t(value >> (8 * i)));
    };
    int arg = 3;
    for (const char *c = format ? format : ""; *c; ++c, ++arg) {
        switch (*c) {
        case 'b': put(uint64_t(int64_t(lua_tonumber(L, arg))), 1); break;
        case 'h': put(uint64_t(int64_t(lua_tonumber(L, arg))), 2); break;
        case 'i': put(uint64_t(int64_t(lua_tonumber(L, arg))), 4); break;
        case 'f': {
            const float value = float(lua_tonumber(L, arg));
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            put(bits, 4);
            break;
        }
        case 'l': put(guidArg(L, arg), 8); break;
        case 'p': {
            const uint64_t guid = guidArg(L, arg);
            uint8_t mask = 0;
            std::vector<uint8_t> bytes;
            for (int i = 0; i < 8; ++i)
                if (const uint8_t b = uint8_t(guid >> (8 * i))) {
                    mask |= uint8_t(1u << i);
                    bytes.push_back(b);
                }
            out.push_back(mask);
            out.insert(out.end(), bytes.begin(), bytes.end());
            break;
        }
        case 's': {
            size_t length = 0;
            const char *text = lua_tolstring(L, arg, &length);
            if (text)
                out.insert(out.end(), text, text + length);
            out.push_back(0);
            break;
        }
        default:
            luaL_error(L, "__AndroidSend: bad format letter");
            return 0;
        }
    }
    std::lock_guard<std::mutex> link(worldLinkMutex());
    if (worldLink().outgoing.size() < 256)
        worldLink().outgoing.push_back(std::move(packet));
    return 0;
}
int lPlayerGuid(lua_State *L) {
    uint64_t guid;
    {
        std::lock_guard<std::mutex> link(worldLinkMutex());
        guid = worldLink().guid;
    }
    if (!guid)
        return 0;
    const auto text = guidText(guid);
    lua_pushstring(L, text.c_str());
    return 1;
}
} // namespace

void uiSetPlayerSnapshots(std::shared_ptr<const PlayerFieldsView> fields, std::shared_ptr<const PlayerDataView> data) {
    if (fields)
        fieldsView = std::move(fields);
    if (data)
        dataView = std::move(data);
}

void uiRegisterDataFunctions() {
    FrameScript_RegisterFunction("__AndroidPlayerField", lPlayerField);
    FrameScript_RegisterFunction("__AndroidItemObject", lItemObject);
    FrameScript_RegisterFunction("__AndroidContainerSlot", lContainerSlot);
    FrameScript_RegisterFunction("__AndroidItemTemplate", lItemTemplate);
    FrameScript_RegisterFunction("__AndroidItemStats", lItemStats);
    FrameScript_RegisterFunction("__AndroidItemSpells", lItemSpells);
    FrameScript_RegisterFunction("__AndroidSpells", lSpells);
    FrameScript_RegisterFunction("__AndroidAction", lAction);
    FrameScript_RegisterFunction("__AndroidSetAction", lSetAction);
    FrameScript_RegisterFunction("__AndroidQuest", lQuest);
    FrameScript_RegisterFunction("__AndroidParty", lParty);
    FrameScript_RegisterFunction("__AndroidObjectiveName", lObjectiveName);
    FrameScript_RegisterFunction("__AndroidSend", lSend);
    FrameScript_RegisterFunction("__AndroidPlayerGuid", lPlayerGuid);
    FrameScript_RegisterFunction("__AndroidDbc", lDbc);
    FrameScript_RegisterFunction("__AndroidDbcRow", lDbcRow);
    FrameScript_RegisterFunction("__AndroidDbcCount", lDbcCount);
}
int uiFactionReaction(uint32_t unitTemplate, uint32_t viewerTemplate) {
    if (!unitTemplate || !viewerTemplate || !uiActive()) // DBCs are read through the UI's mount
        return -1;
    Table *t = table("FactionTemplate");
    if (!t)
        return -1;
    const auto a = t->rows.find(unitTemplate), b = t->rows.find(viewerTemplate);
    if (a == t->rows.end() || b == t->rows.end())
        return -1;
    auto col = [&](uint32_t row, uint32_t c) { return t->data.u32(row, c); };
    const uint32_t faction = col(a->second, 1), flags = col(a->second, 2), group = col(a->second, 3),
                   friendGroup = col(a->second, 4), enemyGroup = col(a->second, 5);
    const uint32_t otherFaction = col(b->second, 1), otherGroup = col(b->second, 3), otherFriendGroup = col(b->second, 4);
    if (otherGroup & enemyGroup)
        return 1;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t enemy = col(a->second, 6 + i);
        if (!enemy)
            break;
        if (enemy == otherFaction)
            return 1;
    }
    if (otherGroup & friendGroup)
        return 4;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t ally = col(a->second, 10 + i);
        if (!ally)
            break;
        if (ally == otherFaction)
            return 4;
    }
    if (group & otherFriendGroup)
        return 4;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t ally = col(b->second, 10 + i);
        if (!ally)
            break;
        if (ally == faction)
            return 4;
    }
    return int((~(flags >> 12) & 2) | 1);
}
} // namespace wow::android
