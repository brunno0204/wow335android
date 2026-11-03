-- Android port: chat windows, chat colors and languages (WoW/Source/Ui/ChatFrame.cpp and
-- ChatFrame_Scripts.cpp behaviour, with the client's default chat-cache state).

-- s_chatTypeInfo: name and default color (r, g, b bytes), in CHAT_MSG order.
local CHAT_TYPES = {
  {"SYSTEM", 255, 255, 0}, {"SAY", 255, 255, 255}, {"PARTY", 170, 170, 255}, {"RAID", 255, 127, 0},
  {"GUILD", 64, 255, 64}, {"OFFICER", 64, 192, 64}, {"YELL", 255, 64, 64}, {"WHISPER", 255, 128, 255},
  {"WHISPER_FOREIGN", 255, 128, 255}, {"WHISPER_INFORM", 255, 128, 255}, {"EMOTE", 255, 128, 64},
  {"TEXT_EMOTE", 255, 128, 64}, {"MONSTER_SAY", 255, 255, 159}, {"MONSTER_PARTY", 170, 170, 255},
  {"MONSTER_YELL", 255, 64, 64}, {"MONSTER_WHISPER", 255, 181, 235}, {"MONSTER_EMOTE", 255, 128, 64},
  {"CHANNEL", 255, 192, 192}, {"CHANNEL_JOIN", 192, 128, 128}, {"CHANNEL_LEAVE", 192, 128, 128},
  {"CHANNEL_LIST", 192, 128, 128}, {"CHANNEL_NOTICE", 192, 192, 192}, {"CHANNEL_NOTICE_USER", 192, 192, 192},
  {"AFK", 255, 128, 255}, {"DND", 255, 128, 255}, {"IGNORED", 255, 0, 0}, {"SKILL", 85, 85, 255},
  {"LOOT", 0, 170, 0}, {"MONEY", 255, 255, 0}, {"OPENING", 128, 128, 255}, {"TRADESKILLS", 255, 255, 255},
  {"PET_INFO", 128, 128, 255}, {"COMBAT_MISC_INFO", 128, 128, 255}, {"COMBAT_XP_GAIN", 111, 111, 255},
  {"COMBAT_HONOR_GAIN", 224, 202, 10}, {"COMBAT_FACTION_CHANGE", 128, 128, 255},
  {"BG_SYSTEM_NEUTRAL", 255, 120, 10}, {"BG_SYSTEM_ALLIANCE", 0, 174, 239}, {"BG_SYSTEM_HORDE", 255, 0, 0},
  {"RAID_LEADER", 255, 72, 9}, {"RAID_WARNING", 255, 72, 0}, {"RAID_BOSS_EMOTE", 255, 221, 0},
  {"RAID_BOSS_WHISPER", 255, 221, 0}, {"FILTERED", 255, 0, 0}, {"BATTLEGROUND", 255, 127, 0},
  {"BATTLEGROUND_LEADER", 255, 219, 183}, {"RESTRICTED", 255, 0, 0}, {"BATTLENET", 255, 255, 255},
  {"ACHIEVEMENT", 255, 255, 0}, {"GUILD_ACHIEVEMENT", 64, 255, 64}, {"ARENA_POINTS", 255, 255, 255},
  {"PARTY_LEADER", 118, 200, 255}, {"TARGETICONS", 255, 255, 0}, {"BN_WHISPER", 0, 255, 246},
  {"BN_WHISPER_INFORM", 0, 255, 246}, {"BN_CONVERSATION", 0, 177, 240}, {"BN_CONVERSATION_NOTICE", 0, 177, 240},
  {"BN_CONVERSATION_LIST", 0, 177, 240}, {"BN_INLINE_TOAST_ALERT", 130, 197, 255},
  {"BN_INLINE_TOAST_BROADCAST", 130, 197, 255}, {"BN_INLINE_TOAST_BROADCAST_INFORM", 130, 197, 255},
  {"BN_INLINE_TOAST_CONVERSATION", 130, 197, 255},
}
local chatColors = {}
for i, t in ipairs(CHAT_TYPES) do chatColors[i] = {name = t[1], r = t[2] / 255, g = t[3] / 255, b = t[4] / 255} end
for i = 1, 10 do
  local c = CHAT_TYPES[18]
  chatColors[#CHAT_TYPES + i] = {name = "CHANNEL" .. i, r = c[2] / 255, g = c[3] / 255, b = c[4] / 255}
end

-- s_chatMessageGroups: name and whether a new chat window shows it by default.
local GROUPS = {
  "SYSTEM", "SYSTEM_NOMENU", "SAY", "EMOTE", "YELL", "WHISPER", "PARTY", "PARTY_LEADER", "RAID",
  "RAID_LEADER", "RAID_WARNING", "BATTLEGROUND", "BATTLEGROUND_LEADER", "GUILD", "OFFICER",
  "MONSTER_SAY", "MONSTER_YELL", "MONSTER_EMOTE", "MONSTER_WHISPER", "MONSTER_BOSS_EMOTE",
  "MONSTER_BOSS_WHISPER", "ERRORS", "AFK", "DND", "IGNORED", "BG_HORDE", "BG_ALLIANCE", "BG_NEUTRAL",
  "COMBAT_FACTION_CHANGE", "SKILL", "LOOT", "MONEY", "CHANNEL", "ACHIEVEMENT", "GUILD_ACHIEVEMENT",
  "TARGETICONS", "BN_WHISPER", "BN_WHISPER_INFORM", "BN_CONVERSATION", "BN_INLINE_TOAST_ALERT",
  "OPENING", "TRADESKILLS", "PET_INFO", "COMBAT_XP_GAIN", "COMBAT_HONOR_GAIN", "COMBAT_MISC_INFO",
}
local GROUP_DEFAULT = {}
for i = 1, #GROUPS do GROUP_DEFAULT[i] = GROUPS[i] ~= "TARGETICONS" end

-- s_chatWindows after the defaults load (ChatWindowInfo::Init + window 1 General, 2 Combat Log).
local windows = {}
for i = 1, 10 do
  local w = {name = "", size = 0, r = 0, g = 0, b = 0, a = 40 / 255, shown = false, locked = true,
             docked = nil, uninteractable = nil, groups = {}, channels = {}, width = 430, height = 120}
  if i == 1 then
    w.shown = true; w.docked = 1
    for g = 1, 40 do w.groups[g] = true end
    w.channels = { {"General", 1}, {"Trade", 2}, {"LocalDefense", 22}, {"LookingForGroup", 26} }
  elseif i == 2 then
    w.shown = true; w.docked = 2
    for g = 41, 46 do w.groups[g] = GROUP_DEFAULT[g] end
  end
  windows[i] = w
end
local function window(index)
  index = tonumber(index)
  return index and windows[floor(index)]
end
local function flag(value) if value then return 1 end end

function GetChatWindowInfo(index)
  local w = window(index)
  if not w then return end
  return w.name, w.size, w.r, w.g, w.b, w.a, flag(w.shown), flag(w.locked), w.docked, w.uninteractable
end
function GetChatWindowMessages(index)
  local w = window(index)
  if not w then return end
  local list = {}
  for g = 1, #GROUPS do if w.groups[g] then list[#list + 1] = GROUPS[g] end end
  return unpack(list)
end
function GetChatWindowChannels(index)
  local w = window(index)
  if not w then return end
  local list = {}
  for _, c in ipairs(w.channels) do list[#list + 1] = c[1]; list[#list + 1] = c[2] end
  return unpack(list)
end
local function groupIndex(name)
  for g = 1, #GROUPS do if GROUPS[g] == name then return g end end
end
function AddChatWindowMessages(index, name)
  local w, g = window(index), groupIndex(name)
  if w and g then w.groups[g] = true end
end
function RemoveChatWindowMessages(index, name)
  local w, g = window(index), groupIndex(name)
  if w and g then w.groups[g] = nil end
end
function AddChatWindowChannel(index, name)
  local w = window(index)
  if not w or not name then return end
  for _, c in ipairs(w.channels) do if c[1] == name then return end end
  w.channels[#w.channels + 1] = {name, 0}
end
function RemoveChatWindowChannel(index, name)
  local w = window(index)
  if not w then return end
  for i, c in ipairs(w.channels) do if c[1] == name then tremove(w.channels, i); return end end
end
function SetChatWindowName(index, name) local w = window(index); if w then w.name = name or "" end end
function SetChatWindowSize(index, size) local w = window(index); if w then w.size = tonumber(size) or 0 end end
function SetChatWindowColor(index, r, g, b)
  local w = window(index)
  if w then w.r, w.g, w.b = tonumber(r) or 0, tonumber(g) or 0, tonumber(b) or 0 end
end
function SetChatWindowAlpha(index, a) local w = window(index); if w then w.a = tonumber(a) or 0 end end
function SetChatWindowLocked(index, v) local w = window(index); if w then w.locked = v and v ~= 0 end end
function SetChatWindowShown(index, v) local w = window(index); if w then w.shown = v and v ~= 0 end end
function SetChatWindowDocked(index, v) local w = window(index); if w then w.docked = tonumber(v) end end
function SetChatWindowUninteractable(index, v)
  local w = window(index); if w then w.uninteractable = (v and v ~= 0) and 1 or nil end
end
function SetChatWindowSavedPosition(index, point, x, y)
  local w = window(index); if w then w.point, w.x, w.y = point, x, y end
end
function GetChatWindowSavedPosition(index)
  local w = window(index)
  if w and w.point then return w.point, w.x, w.y end
end
function SetChatWindowSavedDimensions(index, width, height)
  local w = window(index); if w then w.width, w.height, w.savedSize = width, height, true end
end
function GetChatWindowSavedDimensions(index)
  local w = window(index)
  if w and w.savedSize then return w.width, w.height end
end
function ResetChatWindows() end

function GetChatTypeIndex(kind)
  if type(kind) ~= "string" then return 0 end
  kind = strupper(kind)
  for i, c in ipairs(chatColors) do if c.name == kind then return i end end
  return 0
end
function ChangeChatColor(kind, r, g, b)
  local i = GetChatTypeIndex(kind)
  if i == 0 then return end
  local c = chatColors[i]
  c.r, c.g, c.b = r, g, b
  __AndroidSignal("UPDATE_CHAT_COLOR", c.name, r, g, b)
end
function ResetChatColors() end

-- Languages (Languages.dbc ids): faction default plus the racial one.
local LANGUAGES = { [1] = "Orcish", [2] = "Darnassian", [3] = "Taurahe", [6] = "Dwarvish", [7] = "Common",
                    [10] = "Thalassian", [13] = "Gnomish", [14] = "Troll", [33] = "Gutterspeak", [35] = "Draenei" }
local RACE_LANGUAGES = { [1] = {7}, [2] = {1}, [3] = {7, 6}, [4] = {7, 2}, [5] = {1, 33}, [6] = {1, 3},
                         [7] = {7, 13}, [8] = {1, 14}, [10] = {1, 10}, [11] = {7, 35} }
local function playerLanguages()
  local race = select(10, __AndroidUnit("player"))
  return RACE_LANGUAGES[race or 0] or {0}
end
function GetDefaultLanguage() return LANGUAGES[playerLanguages()[1]] end
function GetNumLanguages() return #playerLanguages() end
function GetLanguageByIndex(index) local id = playerLanguages()[tonumber(index) or 0]; return id and LANGUAGES[id] end
function __AndroidLanguageId(name)
  for id, text in pairs(LANGUAGES) do if text == name then return id end end
  return playerLanguages()[1]
end

-- Channels the server joined us to (General/Trade...): numbers only, no channel list yet.
function GetChannelName(id)
  local names = { [1] = "General", [2] = "Trade", [3] = "LocalDefense", [4] = "LookingForGroup" }
  local n = tonumber(id)
  if n and names[n] then return n, names[n], 0 end
  if type(id) == "string" then for k, v in pairs(names) do if strlower(v) == strlower(id) then return k, v, 0 end end end
  return 0, nil, 0
end
function GetChannelList() return end

-- Chat colors and windows are announced once the interface exists (CGGameUI::Initialize).
function __AndroidChatLoginEvents()
  for _, c in ipairs(chatColors) do __AndroidSignal("UPDATE_CHAT_COLOR", c.name, c.r, c.g, c.b) end
  __AndroidSignal("UPDATE_CHAT_WINDOWS")
end
