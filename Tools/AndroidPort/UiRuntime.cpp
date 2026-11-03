// Original in-game interface on Android: the decompiled UI engine (CSimpleTop, FrameXML,
// FrameScript/Lua, Gxu fonts, textures) loads Interface\FrameXML\FrameXML.toc from the MPQs
// and draws over the 3D world on the GL thread. Game data comes from the native world
// state (WorldLink) through a small API layer (UiPrelude.lua + the __Android* primitives).
#include "UiRuntime.h"
#include "Engine/Source/Base/Coordinate.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Base/Status.h"
#include "Engine/Source/Event/Types.h"
#include "Engine/Source/Frame/CSimpleEditBox.h"
#include "Engine/Source/Frame/CSimpleFrame.h"
#include "Engine/Source/Frame/CSimpleTop.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/FrameXML/FrameXML.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gxu/GxuFont.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "Engine/Source/Scrn/Scrn.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/Storm/Source/MD5.h"
#include "WoW/Source/Ui/Types.h"
#include "WorldLink.h"
#include "CrashReport.h"
#include "UiGameData.h"
#include "UiMinimap.h"
#include "UiModelFrames.h"
#include <android/log.h>
#include <sys/stat.h>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

void RegisterSimpleFrameScriptMethods();
extern int32_t (*g_frameXMLFileHook)(const char *filename, int32_t entering);
CSimpleFrame *Create_SimpleFrame(CSimpleFrame *parent);
CSimpleFrame *Create_SimpleModel(CSimpleFrame *parent);

namespace wow::android {
namespace {
#include "UiApiNames.inc"
#include "UiPrelude.inc"

struct UiUnit {
    bool exists = false;
    uint64_t guid = 0;
    std::string name;
    uint32_t level = 0, health = 0, maxHealth = 0, power = 0, maxPower = 0, flags = 0, npcFlags = 0;
    uint32_t factionTemplate = 0;
    uint8_t powerType = 0, race = 0, cls = 0, sex = 0;
    bool player = false, dead = false;
};
struct UiState {
    bool booted = false, failed = false;
    int width = 1, height = 1;
    uint64_t lastMs = 0, lastSync = 0;
    float fps = 0;
    std::string locale = "enUS", realm, zone;
    std::map<std::string, uint32_t> eventIndex;
    std::vector<const char *> eventNames;
    // Snapshot of the world for the Lua API (refreshed 10 times per second).
    UiUnit self, target;
    std::map<uint64_t, UiUnit> units;
    uint64_t targetGuid = 0;
    uint32_t unitsSerial = 0;
    uint32_t areaId = 0;
    std::shared_ptr<const PlayerFieldsView> playerFields;
    std::shared_ptr<const PlayerDataView> playerData;
};
UiState ui;

uint64_t steadyMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
// Diagnostics shown in the app (overlay line and the "Log da UI" dialog).
std::mutex diagMutex;
std::vector<std::string> diagLines;
size_t diagErrors = 0, diagFiles = 0, diagPaints = 0;
std::string diagLast, diagLastError;
void log(const char *text) {
    __android_log_print(ANDROID_LOG_WARN, "WowPort", "UI: %s", text);
    {
        std::lock_guard<std::mutex> lock(diagMutex);
        std::string line = text ? text : "";
        if (line.size() > 400)
            line.resize(400);
        if (line.rfind("Lua:", 0) == 0 || line.find("rror") != std::string::npos) {
            ++diagErrors;
            diagLastError = line;
        }
        diagLast = line;
        if (diagLines.size() >= 400)
            diagLines.erase(diagLines.begin(), diagLines.begin() + 100);
        diagLines.push_back(std::move(line));
    }
    crashNote(text);
}

UiUnit unitFromView(const UnitView &v) {
    UiUnit u;
    u.exists = true;
    u.guid = v.guid;
    u.name = v.name;
    u.level = v.level;
    u.health = v.health;
    u.maxHealth = v.maxHealth;
    u.power = v.power;
    u.maxPower = v.maxPower;
    u.powerType = v.powerType;
    u.race = v.race;
    u.cls = v.cls;
    u.sex = v.sex;
    u.player = v.type == ObjectType::Player;
    u.dead = v.dead;
    u.flags = v.flags;
    u.npcFlags = v.npcFlags;
    u.factionTemplate = v.factionTemplate;
    return u;
}
const UiUnit *resolve(const char *token) {
    if (!token)
        return nullptr;
    const std::string t(token);
    if (t == "player")
        return ui.self.exists ? &ui.self : nullptr;
    if (t == "target")
        return ui.target.exists ? &ui.target : nullptr;
    if (t.rfind("0x", 0) == 0) { // a GUID string
        const uint64_t guid = std::strtoull(t.c_str() + 2, nullptr, 16);
        if (guid == ui.self.guid)
            return &ui.self;
        auto found = ui.units.find(guid);
        return found == ui.units.end() ? nullptr : &found->second;
    }
    return nullptr;
}
std::string guidString(uint64_t guid) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%016llX", static_cast<unsigned long long>(guid));
    return text;
}
int lUnit(lua_State *L) {
    const UiUnit *u = resolve(lua_tostring(L, 1));
    if (!u) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushboolean(L, 1);
    lua_pushstring(L, guidString(u->guid).c_str());
    lua_pushstring(L, u->name.empty() ? "?" : u->name.c_str());
    lua_pushnumber(L, u->level);
    lua_pushnumber(L, u->health);
    lua_pushnumber(L, u->maxHealth);
    lua_pushnumber(L, u->power);
    lua_pushnumber(L, u->maxPower);
    lua_pushnumber(L, u->powerType);
    lua_pushnumber(L, u->race);
    lua_pushnumber(L, u->cls);
    lua_pushnumber(L, u->sex);
    lua_pushboolean(L, u->player);
    lua_pushboolean(L, u->dead);
    lua_pushnumber(L, u->flags);
    lua_pushnumber(L, u->npcFlags);
    // Reaction toward the player (FactionTemplateReaction); nil when unknown.
    const int reaction = u == &ui.self || u->guid == ui.self.guid ? 4 : uiFactionReaction(u->factionTemplate, ui.self.factionTemplate);
    if (reaction >= 0)
        lua_pushnumber(L, reaction);
    else
        lua_pushnil(L);
    return 17;
}
int lTime(lua_State *L) {
    lua_pushnumber(L, double(steadyMs()) / 1000.0);
    return 1;
}
int lFramerate(lua_State *L) {
    lua_pushnumber(L, ui.fps);
    return 1;
}
int lScreen(lua_State *L) {
    // UIParent space: 768 units high, width by aspect (the original GetScreenWidth/Height).
    const double aspect = double(ui.width) / double(std::max(1, ui.height));
    lua_pushnumber(L, 768.0 * aspect);
    lua_pushnumber(L, 768.0);
    return 2;
}
int lPixels(lua_State *L) {
    lua_pushnumber(L, ui.width);
    lua_pushnumber(L, ui.height);
    return 2;
}
int lLocale(lua_State *L) {
    lua_pushstring(L, ui.locale.c_str());
    return 1;
}
int lRealm(lua_State *L) {
    lua_pushstring(L, ui.realm.c_str());
    return 1;
}
int lZone(lua_State *L) {
    lua_pushstring(L, ui.zone.c_str());
    return 1;
}
int lLog(lua_State *L) {
    log(lua_tostring(L, 1) ? lua_tostring(L, 1) : "(nil)");
    return 0;
}
int lTarget(lua_State *L) {
    uint64_t guid = 0;
    if (const char *text = lua_tostring(L, 1); text && text[0] == '0' && text[1] == 'x')
        guid = std::strtoull(text + 2, nullptr, 16);
    std::lock_guard<std::mutex> link(worldLinkMutex());
    auto &state = worldLink();
    state.target = guid;
    ++state.targetSerial;
    return 0;
}
// __AndroidInteract(guid): talk/loot/use like a right click on the object (InteractUnit).
int lInteract(lua_State *L) {
    uint64_t guid = 0;
    if (const char *text = lua_tostring(L, 1); text && text[0] == '0' && text[1] == 'x')
        guid = std::strtoull(text + 2, nullptr, 16);
    if (!guid)
        return 0;
    std::lock_guard<std::mutex> link(worldLinkMutex());
    auto &state = worldLink();
    state.interact = guid;
    ++state.interactSerial;
    return 0;
}
std::string dataDirectory;
// Saved-variable file names: letters, digits, '-', '_' and '.', under <data>/WTF/.
std::string savedPath(const char *name) {
    if (dataDirectory.empty() || !name || !*name)
        return {};
    std::string clean;
    for (const char *c = name; *c; ++c)
        clean += (std::isalnum(static_cast<unsigned char>(*c)) || *c == '-' || *c == '_' || *c == '.') ? *c : '_';
    if (clean.find("..") != std::string::npos)
        return {};
    return dataDirectory + "/WTF/" + clean;
}
// __AndroidSaveFile(name, text) -> true when written (atomic rename).
int lSaveFile(lua_State *L) {
    const std::string path = savedPath(lua_tostring(L, 1));
    size_t length = 0;
    const char *text = lua_tolstring(L, 2, &length);
    if (path.empty() || !text)
        return 0;
    ::mkdir((dataDirectory + "/WTF").c_str(), 0700);
    const std::string temp = path + ".tmp";
    FILE *file = std::fopen(temp.c_str(), "wb");
    if (!file)
        return 0;
    const bool ok = std::fwrite(text, 1, length, file) == length;
    std::fclose(file);
    if (!ok || std::rename(temp.c_str(), path.c_str()) != 0)
        return 0;
    lua_pushboolean(L, 1);
    return 1;
}
// __AndroidLoadFile(name) -> text or nil.
int lLoadFile(lua_State *L) {
    const std::string path = savedPath(lua_tostring(L, 1));
    if (path.empty())
        return 0;
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return 0;
    std::string text;
    char buffer[8192];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        text.append(buffer, n);
    std::fclose(file);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}
std::mutex audioMutex;
std::string audioQueue;
std::vector<std::string> scriptQueue; // guarded by audioMutex
// __AndroidAudio(fields...): one command line for the Java audio player.
int lAudio(lua_State *L) {
    std::string line;
    for (int i = 1; i <= lua_gettop(L); ++i) {
        const char *text = lua_tostring(L, i);
        if (i > 1)
            line += '|';
        if (text)
            for (const char *c = text; *c; ++c)
                line += (*c == '\n' || *c == '|') ? ' ' : *c;
    }
    std::lock_guard<std::mutex> lock(audioMutex);
    if (audioQueue.size() < 64 * 1024)
        audioQueue += line + '\n';
    return 0;
}
void logStatus(CStatus &status);
// __AndroidReadFile(path): file text from the archives (addon .toc metadata), or nil.
int lReadFile(lua_State *L) {
    const char *path = lua_tostring(L, 1);
    void *data = nullptr;
    uint32_t size = 0;
    if (!path || !SFile::ReadFile(nullptr, path, &data, &size, 0, 1, nullptr) || !data)
        return 0;
    lua_pushlstring(L, static_cast<const char *>(data), size);
    SFile::Unload(data);
    return 1;
}
// __AndroidLoadToc(tocPath, addonName): FrameXML_CreateFrames for an addon (AddOns.cpp LoadAddOn).
int lLoadToc(lua_State *L) {
    const char *toc = lua_tostring(L, 1);
    const char *addon = lua_tostring(L, 2);
    if (!toc)
        return 0;
    CStatus status;
    MD5_CTX md5;
    MD5Init(&md5);
    const int32_t created = FrameXML_CreateFrames(toc, addon, &md5, &status);
    logStatus(status);
    lua_pushboolean(L, created != 0);
    return 1;
}
// __AndroidSignal(event, ...): fire an original script event from the Lua game layer.
int lSignal(lua_State *L) {
    const char *name = lua_tostring(L, 1);
    if (!name)
        return 0;
    const auto found = ui.eventIndex.find(name);
    if (found == ui.eventIndex.end())
        return 0;
    FrameScript_SignalEvent(found->second, L, lua_gettop(L));
    return 0;
}
void signal(const char *name, const char *unit = nullptr) {
    const auto found = ui.eventIndex.find(name);
    if (found == ui.eventIndex.end())
        return;
    if (unit)
        FrameScript_SignalEvent(found->second, "%s", unit);
    else
        FrameScript_SignalEvent(found->second, nullptr);
}
const char *powerEvent(uint8_t type, bool max) {
    static const char *const current[] = {"UNIT_MANA", "UNIT_RAGE", "UNIT_FOCUS", "UNIT_ENERGY",
                                          "UNIT_HAPPINESS", "UNIT_RUNIC_POWER", "UNIT_RUNIC_POWER"};
    static const char *const maximum[] = {"UNIT_MAXMANA", "UNIT_MAXRAGE", "UNIT_MAXFOCUS", "UNIT_MAXENERGY",
                                          "UNIT_MAXHAPPINESS", "UNIT_MAXRUNIC_POWER", "UNIT_MAXRUNIC_POWER"};
    return (max ? maximum : current)[type < 7 ? type : 0];
}
void compare(const UiUnit &before, const UiUnit &after, const char *token) {
    if (before.health != after.health)
        signal("UNIT_HEALTH", token);
    if (before.maxHealth != after.maxHealth)
        signal("UNIT_MAXHEALTH", token);
    if (before.powerType != after.powerType)
        signal("UNIT_DISPLAYPOWER", token);
    if (before.power != after.power)
        signal(powerEvent(after.powerType, false), token);
    if (before.maxPower != after.maxPower)
        signal(powerEvent(after.powerType, true), token);
    if (before.level != after.level)
        signal("UNIT_LEVEL", token);
    if (before.name != after.name)
        signal("UNIT_NAME_UPDATE", token);
}
const char *chatEvent(uint8_t type) {
    static const char *const names[] = {
        "SYSTEM", "SAY", "PARTY", "RAID", "GUILD", "OFFICER", "YELL", "WHISPER", "WHISPER", "WHISPER_INFORM",
        "EMOTE", "TEXT_EMOTE", "MONSTER_SAY", "MONSTER_PARTY", "MONSTER_YELL", "MONSTER_WHISPER",
        "MONSTER_EMOTE", "CHANNEL", "CHANNEL_JOIN", "CHANNEL_LEAVE", "CHANNEL_LIST", "CHANNEL_NOTICE",
        "CHANNEL_NOTICE_USER", "AFK", "DND", "IGNORED", "SKILL", "LOOT", "MONEY", "OPENING", "TRADESKILLS",
        "PET_INFO", "COMBAT_MISC_INFO", "COMBAT_XP_GAIN", "COMBAT_HONOR_GAIN", "COMBAT_FACTION_CHANGE",
        "BG_SYSTEM_NEUTRAL", "BG_SYSTEM_ALLIANCE", "BG_SYSTEM_HORDE", "RAID_LEADER", "RAID_WARNING",
        "RAID_BOSS_EMOTE", "RAID_BOSS_WHISPER", "FILTERED", "BATTLEGROUND", "BATTLEGROUND_LEADER",
        "RESTRICTED", "BN_WHISPER", "ACHIEVEMENT", "GUILD_ACHIEVEMENT", "ARENA_POINTS", "PARTY_LEADER"};
    return type < sizeof(names) / sizeof(names[0]) ? names[type] : "SYSTEM";
}
uint32_t chatLineId = 0;
void deliverChat(const std::vector<ChatLine> &lines) {
    for (const auto &line : lines) {
        const std::string event = std::string("CHAT_MSG_") + chatEvent(line.type);
        const auto found = ui.eventIndex.find(event);
        if (found == ui.eventIndex.end())
            continue;
        const std::string channel = line.channel.empty() ? "" : "1. " + line.channel;
        const std::string guid = line.sender ? guidString(line.sender) : "";
        // arg1 text, arg2 author, arg3 language, arg4 channel, arg5 target, arg6 flags,
        // arg7 zone channel id, arg8 channel number, arg9 channel name, arg10, arg11 line id, arg12 guid.
        FrameScript_SignalEvent(found->second, "%s%s%s%s%s%s%d%d%s%d%d%s", line.text.c_str(), line.author.c_str(),
                                "", channel.c_str(), "", "", 0, line.channel.empty() ? 0 : 1,
                                line.channel.c_str(), 0, int32_t(++chatLineId), guid.c_str());
    }
}
// Copies the world state and fires the matching original script events.
void runQueuedScripts() {
    std::vector<std::string> scripts;
    {
        std::lock_guard<std::mutex> lock(audioMutex);
        scripts.swap(scriptQueue);
    }
    for (const auto &script : scripts)
        FrameScript_Execute(script.c_str(), "AndroidCommand.lua", nullptr);
}
void sync(bool initial) {
    UiUnit self, target;
    std::map<uint64_t, UiUnit> units;
    uint64_t targetGuid = 0;
    std::vector<ChatLine> chat;
    std::shared_ptr<const PlayerFieldsView> playerFields;
    std::shared_ptr<const PlayerDataView> playerData;
    std::vector<std::vector<std::string>> scriptEvents;
    {
        std::lock_guard<std::mutex> link(worldLinkMutex());
        auto &state = worldLink();
        playerFields = state.playerFields;
        playerData = state.playerData;
        scriptEvents.swap(state.scriptEvents);
        if (!initial && state.areaId != ui.areaId) {
            ui.areaId = state.areaId;
            scriptEvents.push_back({"__ANDROID_AREA", std::to_string(state.areaId)});
        }
        self.exists = state.active;
        self.guid = state.guid;
        self.name = state.playerName;
        self.level = state.hud.level;
        self.health = state.hud.health;
        self.maxHealth = state.hud.maxHealth;
        self.power = state.hud.power;
        self.maxPower = state.hud.maxPower;
        self.powerType = state.hud.powerType;
        self.factionTemplate = state.hud.factionTemplate;
        self.race = state.race;
        self.cls = state.cls;
        self.sex = state.sex;
        self.player = true;
        self.dead = self.maxHealth && !self.health;
        targetGuid = state.target;
        chat.swap(state.chat);
        if (state.unitsSerial != ui.unitsSerial || initial) {
            for (const auto &v : state.units)
                if (v.type != ObjectType::GameObject)
                    units[v.guid] = unitFromView(v);
            ui.unitsSerial = state.unitsSerial;
        } else
            units = ui.units;
    }
    if (targetGuid == self.guid && self.guid)
        target = self;
    else if (auto found = units.find(targetGuid); found != units.end())
        target = found->second;
    const UiUnit oldSelf = ui.self, oldTarget = ui.target;
    const uint64_t oldTargetGuid = ui.targetGuid;
    ui.self = self;
    ui.target = target;
    ui.units = std::move(units);
    ui.targetGuid = target.exists ? targetGuid : 0;
    // Player data: which categories changed since the last sync (Lua fires the events).
    std::vector<const char *> changes;
    if (playerFields && playerFields != ui.playerFields) {
        if (!ui.playerFields || playerFields->fieldsSerial != ui.playerFields->fieldsSerial)
            changes.push_back("fields");
        if (!ui.playerFields || playerFields->itemsSerial != ui.playerFields->itemsSerial)
            changes.push_back("items");
        ui.playerFields = playerFields;
    }
    if (playerData && playerData != ui.playerData) {
        const auto *old = ui.playerData.get();
        if (!old || playerData->spellsSerial != old->spellsSerial)
            changes.push_back("spells");
        if (!old || playerData->actionsSerial != old->actionsSerial)
            changes.push_back("actions");
        if (!old || playerData->questsSerial != old->questsSerial)
            changes.push_back("quests");
        if (!old || playerData->partySerial != old->partySerial)
            changes.push_back("party");
        if (!old || playerData->itemInfoSerial != old->itemInfoSerial)
            changes.push_back("iteminfo");
        ui.playerData = playerData;
    }
    if (!changes.empty())
        uiSetPlayerSnapshots(playerFields, playerData);
    if (initial)
        return;
    if (!changes.empty()) {
        lua_State *L = FrameScript_GetContext();
        lua_getglobal(L, "__AndroidPlayerDataChanged");
        if (lua_isfunction(L, -1)) {
            for (const char *change : changes)
                lua_pushstring(L, change);
            if (lua_pcall(L, int(changes.size()), 0, 0) != 0) {
                log(lua_tostring(L, -1) ? lua_tostring(L, -1) : "player data error");
                lua_pop(L, 1);
            }
        } else
            lua_pop(L, 1);
    }
    deliverChat(chat);
    runQueuedScripts();
    // Server notifications: __AndroidServerEvent(name, args...) (Lua maps or fires them).
    for (const auto &event : scriptEvents) {
        if (event.empty())
            continue;
        lua_State *L = FrameScript_GetContext();
        lua_getglobal(L, "__AndroidServerEvent");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        for (const auto &arg : event)
            lua_pushstring(L, arg.c_str());
        if (lua_pcall(L, int(event.size()), 0, 0) != 0) {
            log(lua_tostring(L, -1) ? lua_tostring(L, -1) : "server event error");
            lua_pop(L, 1);
        }
    }
    compare(oldSelf, ui.self, "player");
    if (ui.targetGuid != oldTargetGuid)
        signal("PLAYER_TARGET_CHANGED");
    else if (ui.target.exists)
        compare(oldTarget, ui.target, "target");
}
CSimpleFrame *createPlainFrame(CSimpleFrame *parent) { return Create_SimpleFrame(parent); }
void logStatus(CStatus &status) {
    int shown = 0;
    for (auto *entry = status.m_statusList.Head(); entry && shown < 200;
         entry = status.m_statusList.Next(entry), ++shown)
        if (entry->m_text)
            log(entry->m_text);
}
// FrameXML files in progress (Include nests them) and files skipped after a crash in them.
std::vector<std::string> xmlStack;
std::vector<std::string> skippedFiles;
std::string lowerPath(const char *path) {
    std::string text = path ? path : "";
    for (char &c : text)
        c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
int32_t frameXMLFile(const char *file, int32_t entering) {
    if (entering) {
        const std::string key = lowerPath(file);
        for (const auto &skipped : skippedFiles)
            if (key == skipped) {
                std::string text = std::string("arquivo ignorado (fechou o jogo antes): ") + (file ? file : "");
                log(text.c_str());
                return 0;
            }
        xmlStack.emplace_back(file ? file : "");
        ++diagFiles;
    } else if (!xmlStack.empty())
        xmlStack.pop_back();
    crashBreadcrumb2("UI boot: FrameXML ", xmlStack.empty() ? "(toc)" : xmlStack.back().c_str());
    return 1;
}
} // namespace

bool uiActive() { return ui.booted; }
bool uiTextFocus() { return ui.booted && CSimpleEditBox::s_currentFocus != nullptr; }
bool uiPointerOverFrame() {
    if (!ui.booted || !CSimpleTop::s_instance)
        return false;
    CSimpleFrame *focus = CSimpleTop::s_instance->m_mouseFocus;
    if (!focus)
        return false;
    const char *name = focus->GetName();
    return !(name && (!std::strcmp(name, "WorldFrame") || !std::strcmp(name, "UIParent")));
}
bool uiBoot(int width, int height, const std::string &locale, const std::string &realm, std::string &error) {
    if (ui.booted)
        return true;
    if (ui.failed) {
        error = "A interface original falhou ao iniciar antes.";
        return false;
    }
    ui.failed = true; // cleared on success; a crash-free failure is not retried
    ui.width = std::max(1, width);
    ui.height = std::max(1, height);
    ui.locale = locale.empty() ? "enUS" : locale;
    ui.realm = realm;
    try {
        crashBreadcrumb("UI boot: texturas");
        CoordinateSetAspectRatio(float(ui.width) / float(ui.height));
        TextureInitialize();
        crashBreadcrumb("UI boot: fontes");
        GxuFontInitialize();
        crashBreadcrumb("UI boot: Lua");
        FrameScript_Initialize(0);
        RegisterSimpleFrameScriptMethods();
        // Original event table (WoW/Source/Ui/Types.h SCRIPT_EVENTS).
        ui.eventNames.assign(NUM_SCRIPT_EVENTS, "");
#define UI_EVENT(name, index) ui.eventNames[index] = #name;
#define UI_EVENT_ALIAS(name, string, index) ui.eventNames[index] = string;
        SCRIPT_EVENTS(UI_EVENT, UI_EVENT_ALIAS)
#undef UI_EVENT_ALIAS
#undef UI_EVENT
        for (uint32_t i = 0; i < ui.eventNames.size(); ++i)
            if (*ui.eventNames[i] && !ui.eventIndex.count(ui.eventNames[i]))
                ui.eventIndex[ui.eventNames[i]] = i;
        FrameScript_CreateEvents(ui.eventNames.data(), uint32_t(ui.eventNames.size()));
        // Native primitives used by the prelude.
        FrameScript_RegisterFunction("__AndroidUnit", lUnit);
        FrameScript_RegisterFunction("__AndroidTime", lTime);
        FrameScript_RegisterFunction("__AndroidFramerate", lFramerate);
        FrameScript_RegisterFunction("__AndroidScreen", lScreen);
        FrameScript_RegisterFunction("__AndroidPixels", lPixels);
        FrameScript_RegisterFunction("__AndroidLocale", lLocale);
        FrameScript_RegisterFunction("__AndroidRealm", lRealm);
        FrameScript_RegisterFunction("__AndroidZone", lZone);
        FrameScript_RegisterFunction("__AndroidLog", lLog);
        FrameScript_RegisterFunction("__AndroidTarget", lTarget);
        FrameScript_RegisterFunction("__AndroidInteract", lInteract);
        FrameScript_RegisterFunction("__AndroidSaveFile", lSaveFile);
        FrameScript_RegisterFunction("__AndroidAudio", lAudio);
        FrameScript_RegisterFunction("__AndroidLoadFile", lLoadFile);
        FrameScript_RegisterFunction("__AndroidSignal", lSignal);
        FrameScript_RegisterFunction("__AndroidReadFile", lReadFile);
        FrameScript_RegisterFunction("__AndroidLoadToc", lLoadToc);
        uiRegisterDataFunctions();
        uiMinimapRegister();
        uiModelFramesRegister();
        lua_State *L = FrameScript_GetContext();
        lua_newtable(L);
        int index = 1;
        for (const char *name : kOriginalGlobalApi) {
            lua_pushstring(L, name);
            lua_rawseti(L, -2, index++);
        }
        lua_setglobal(L, "__AndroidApiNames");
        lua_newtable(L);
        index = 1;
        for (const char *name : kOriginalFrameMethods) {
            lua_pushstring(L, name);
            lua_rawseti(L, -2, index++);
        }
        lua_setglobal(L, "__AndroidFrameMethods");
        // Frame types: the engine's, plus game types without a native implementation yet
        // created as plain frames/models so FrameXML can instantiate them.
        FrameXML_RegisterDefault();
        for (const char *type : {"WorldFrame", "GameTooltip", "Cooldown", "Minimap", "QuestPOIFrame"})
            FrameXML_RegisterFactory(type, createPlainFrame, false);
        for (const char *type : {"PlayerModel", "DressUpModel", "TabardModel"})
            FrameXML_RegisterFactory(type, createUiModelFrame, false);
        crashBreadcrumb("UI boot: CSimpleTop");
        new CSimpleTop();
        EVENT_DATA_SIZE size{ui.width, ui.height};
        CSimpleTop::OnDisplaySizeChanged(&size, CSimpleTop::s_instance);
        crashBreadcrumb("UI boot: prelude Lua");
        FrameScript_Execute(
            "function __AndroidErrorHandler(m)\n"
            "  local stack = debugstack and debugstack(3, 4, 0) or ''\n"
            "  stack = gsub(gsub(stack, 'Interface\\\\FrameXML\\\\', ''), '\\n', ' | ')\n"
            "  __AndroidLog('Lua: ' .. tostring(m) .. (stack ~= '' and (' @ ' .. stack) or ''))\n"
            "  return m\n"
            "end\n"
            "seterrorhandler(__AndroidErrorHandler)",
            "AndroidErrors.lua", nullptr);
        for (const auto &script : kUiScripts)
            FrameScript_Execute(script.source, script.name, nullptr);
        crashBreadcrumb("UI boot: sync inicial");
        sync(true);
        CStatus status;
        MD5_CTX md5;
        MD5Init(&md5);
        FrameXML_FreeHashNodes();
        g_frameXMLFileHook = frameXMLFile;
        const int32_t created = FrameXML_CreateFrames("Interface\\FrameXML\\FrameXML.toc", nullptr, &md5, &status);
        g_frameXMLFileHook = nullptr;
        crashBreadcrumb("UI boot: eventos de login");
        logStatus(status);
        if (!created)
            log("FrameXML_CreateFrames reported failure (continuing with the frames created)");
        ui.booted = true;
        ui.failed = false;
        ui.lastMs = steadyMs();
        // Login sequence events (UiPrelude.lua __AndroidLoginEvents), as CGGameUI sends them.
        FrameScript_Execute("if __AndroidLoginEvents then __AndroidLoginEvents() end", "AndroidLogin.lua", nullptr);
        FrameScript_Execute(R"lua(
local ok, text = pcall(function()
  local function d(n)
    local f = _G[n]
    if not f then return n .. "=nil" end
    return string.format("%s %s %.0fx%.0f alpha %.2f", n, f:IsVisible() and "visível" or "oculto",
      f:GetWidth() or -1, f:GetHeight() or -1, f:GetAlpha() or -1)
  end
  return d("UIParent") .. "; " .. d("PlayerFrame") .. "; " .. d("MainMenuBar") .. "; " .. d("ChatFrame1")
end)
__AndroidLog("quadros: " .. tostring(text))
)lua", "AndroidDiag.lua", nullptr);
        crashBreadcrumb("UI: iniciada");
        return true;
    } catch (const std::exception &failure) {
        error = failure.what();
        log(error.c_str());
        return false;
    }
}
void uiResize(int width, int height) {
    ui.width = std::max(1, width);
    ui.height = std::max(1, height);
    if (!ui.booted)
        return;
    CoordinateSetAspectRatio(float(ui.width) / float(ui.height));
    EVENT_DATA_SIZE size{ui.width, ui.height};
    CSimpleTop::OnDisplaySizeChanged(&size, CSimpleTop::s_instance);
}
void uiPaint() {
    if (!ui.booted)
        return;
    const uint64_t now = steadyMs();
    const float elapsed = float(std::min<uint64_t>(now - ui.lastMs, 250)) / 1000.0f;
    if (elapsed > 0)
        ui.fps = ui.fps * .9f + .1f / elapsed;
    ui.lastMs = now;
    if (now - ui.lastSync >= 100) {
        ui.lastSync = now;
        crashBreadcrumb("UI: eventos (sync)");
        sync(false);
    }
    crashBreadcrumb("UI: minimapa");
    uiMinimapUpdate(now);
    crashBreadcrumb("UI: desenho/OnUpdate");
    ++diagPaints;
    // The world pass leaves its last model's world/texture transforms and render states
    // (culling, second texture stage, texgen...) on the device; CSimpleRender only sets
    // fog/depth/blend. Paint the layers from a clean UI state, as CGxDevice::ICursorDraw does.
    g_theGxDevicePtr->RsPush();
    GxRsSet(GxRs_PolygonOffset, 0);
    GxRsSet(GxRs_NormalizeNormals, 0);
    GxRsSet(GxRs_Lighting, 0);
    GxRsSet(GxRs_Fog, 0);
    GxRsSet(GxRs_DepthTest, 0);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_ColorWrite, 15);
    GxRsSet(GxRs_Culling, 0);
    GxRsSet(GxRs_ClipPlaneMask, 0);
    GxRsSet(GxRs_Texture1, nullptr);
    GxRsSet(GxRs_ColorOp0, GxTexOp_Mod);
    GxRsSet(GxRs_AlphaOp0, GxTexOp_Mod);
    GxRsSet(GxRs_TexGen0, 0);
    GxRsSet(GxRs_TexGen1, 0);
    GxRsSet(GxRs_TextureCoord0, 0);
    GxRsSet(GxRs_TextureShader0, 0);
    GxRsSet(GxRs_TextureShader1, 0);
    GxRsSet(GxRs_VertexShader, nullptr);
    GxRsSet(GxRs_PixelShader, nullptr);
    const C44Matrix identity;
    GxXformPush(GxXform_World, identity);
    GxXformPush(GxXform_Tex0, identity);
    GxXformPush(GxXform_Tex1, identity);
    ScrnPaintLayersNoPresent(elapsed);
    GxXformPop(GxXform_Tex1);
    GxXformPop(GxXform_Tex0);
    GxXformPop(GxXform_World);
    g_theGxDevicePtr->RsPop();
}
std::string uiDiagnostics() {
    std::lock_guard<std::mutex> lock(diagMutex);
    char text[160];
    if (!diagErrors) {
        std::snprintf(text, sizeof(text), "ok (%zu arquivos)", diagFiles);
        return text;
    }
    std::snprintf(text, sizeof(text), "%zu erros de script", diagErrors);
    std::string result = text;
    if (!diagLastError.empty())
        result += " · último: " + diagLastError.substr(0, 100);
    return result;
}
void uiLog(const std::string &line) { log(line.c_str()); }
std::string uiLogText() {
    std::lock_guard<std::mutex> lock(diagMutex);
    std::string result;
    for (const auto &line : diagLines)
        result += line + "\n";
    return result;
}
void uiSetSkippedFiles(const std::string &list) {
    skippedFiles.clear();
    size_t start = 0;
    while (start < list.size()) {
        size_t end = list.find('\n', start);
        if (end == std::string::npos)
            end = list.size();
        if (end > start)
            skippedFiles.push_back(lowerPath(list.substr(start, end - start).c_str()));
        start = end + 1;
    }
}
void uiSetDataDirectory(const std::string &directory) { dataDirectory = directory; }
void uiQueueScript(const std::string &script) {
    std::lock_guard<std::mutex> lock(audioMutex);
    if (scriptQueue.size() < 16)
        scriptQueue.push_back(script);
}
std::string uiTakeAudio() {
    std::lock_guard<std::mutex> lock(audioMutex);
    std::string out;
    out.swap(audioQueue);
    return out;
}
void uiSetZone(const std::string &zone) {
    if (zone == ui.zone)
        return;
    ui.zone = zone;
    if (ui.booted) {
        signal("ZONE_CHANGED_NEW_AREA");
        signal("ZONE_CHANGED");
    }
}
} // namespace wow::android
