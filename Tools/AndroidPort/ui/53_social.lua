-- Android port: death and resurrection, friends/ignore, /who, guild roster and the zone and
-- subzone names (AreaTable) from the server packets and the terrain under the player.
local signal = __AndroidSignal
local Dbc = __AndroidDbc
local Field = __AndroidPlayerField
local H = __AndroidCombatHandlers
local function chat(text) signal("CHAT_MSG_SYSTEM", text, "", "", "", "", "", 0, 0, "", 0, 0, "") end
local function playerGuid() return __AndroidPlayerGuid() end
local CLASS_FILE = { [1] = "WARRIOR", [2] = "PALADIN", [3] = "HUNTER", [4] = "ROGUE", [5] = "PRIEST",
                     [6] = "DEATHKNIGHT", [7] = "SHAMAN", [8] = "MAGE", [9] = "WARLOCK", [11] = "DRUID" }
local function className(id)
  local file = CLASS_FILE[tonumber(id) or 0]
  return file and (LOCALIZED_CLASS_NAMES_MALE and LOCALIZED_CLASS_NAMES_MALE[file]) or file or "", file
end
local function areaName(id)
  id = tonumber(id) or 0
  if id <= 0 then return "" end
  return Dbc("AreaTable", id, "areaName") or ""
end
__AndroidAreaName = areaName

-- ---- Zone / subzone: SMSG_INIT_WORLD_STATES gives the zone, the terrain the area under the player.
local zone, area = 0, 0
local function zoneOf(id)
  local parent = Dbc("AreaTable", id, "parentAreaID") or 0
  if parent > 0 then return parent end
  return id
end
local function setArea(newZone, newArea)
  local oldZone, oldArea = zone, area
  zone, area = newZone or zone, newArea or area
  if zone ~= oldZone then
    signal("ZONE_CHANGED_NEW_AREA")
    if MinimapZoneText then signal("MINIMAP_ZONE_CHANGED") end
  elseif area ~= oldArea then
    signal("ZONE_CHANGED")
    signal("MINIMAP_ZONE_CHANGED")
  end
end
H.__ANDROID_ZONE = function(map, z, a)
  z, a = tonumber(z) or 0, tonumber(a) or 0
  if z > 0 then setArea(z, a > 0 and a or z) end
end
H.__ANDROID_AREA = function(id)
  id = tonumber(id) or 0
  if id <= 0 then return end
  setArea(zoneOf(id), id)
end
function GetZoneText() local n = areaName(zone); if n ~= "" then return n end; return __AndroidZone() end
function GetRealZoneText() return GetZoneText() end
function GetSubZoneText() if area ~= zone then return areaName(area) end; return "" end
function GetMinimapZoneText() local s = GetSubZoneText(); if s ~= "" then return s end; return GetZoneText() end
-- pvpType, isSubZonePvP, controlling faction name (FACTION_CONTROLLED_TERRITORY needs it).
function GetZonePVPInfo()
  if zone == 0 then return nil end
  local flags = Dbc("AreaTable", area > 0 and area or zone, "flags") or 0
  if bit.band(flags, 0x800) ~= 0 then return "sanctuary", nil, nil end
  local mask = Dbc("AreaTable", zone, "factionGroupMask") or 0
  local horde = UnitFactionGroup("player") == "Horde"
  -- AreaTable.factionGroupMask: 2 Alliance, 4 Horde territory.
  if mask == 2 then return horde and "hostile" or "friendly", nil, FACTION_ALLIANCE or "Alliance" end
  if mask == 4 then return horde and "friendly" or "hostile", nil, FACTION_HORDE or "Horde" end
  return "contested", nil, nil
end
function IsResting() if bit.band(Field(0x96) or 0, 0x20) ~= 0 then return 1 end end

-- ---- Death: health 0 -> PLAYER_DEAD; ghost flag (PLAYER_FLAGS 0x10) -> PLAYER_ALIVE/UNGHOST.
local dead, ghost = false, false
local deathTime, reclaimDelay = 0, 0
local corpse
local resurrecter, spiritHealer
local function isGhost() return bit.band(Field(0x96) or 0, 0x10) ~= 0 end
function UnitIsGhost(unit)
  if (unit == nil or UnitIsUnit(unit, "player")) and isGhost() then return 1 end
end
function UnitIsDeadOrGhost(unit) if UnitIsDead(unit) or UnitIsGhost(unit) then return 1 end end
function RepopMe() __AndroidSend(OP.CMSG_REPOP_REQUEST, "b", 0) end
function GetReleaseTimeRemaining()
  if not dead then return 0 end
  return math.max(0, 360 - (GetTime() - deathTime))
end
function HasSoulstone() return nil end
function CanUseSoulstone() return nil end
function UseSoulstone() end
function IsSelfResurrect() return nil end
function GetCorpseRecoveryDelay()
  if reclaimDelay <= 0 then return 0 end
  return math.max(0, math.ceil(reclaimDelay - GetTime()))
end
function RetrieveCorpse() __AndroidSend(OP.CMSG_RECLAIM_CORPSE, "l", playerGuid()) end
function GetCorpseMapPosition() return 0, 0 end
function AcceptResurrect() if resurrecter then __AndroidSend(OP.CMSG_RESURRECT_RESPONSE, "lb", resurrecter, 1); resurrecter = nil end end
function DeclineResurrect() if resurrecter then __AndroidSend(OP.CMSG_RESURRECT_RESPONSE, "lb", resurrecter, 0); resurrecter = nil end end
function ResurrectHasSickness() return nil end
function ResurrectHasTimer() return nil end
function ResurrectGetOfferer() return resurrecter end
function AcceptXPLoss() if spiritHealer then __AndroidSend(OP.CMSG_SPIRIT_HEALER_ACTIVATE, "l", spiritHealer) end end
function CheckSpiritHealerDist() return 1 end
function CheckBinderDist() return 1 end
function GetBindLocation() return GetZoneText() end
function ConfirmBinder() end
H.__ANDROID_RESURRECT_REQUEST = function(guid, name)
  resurrecter = guid
  signal("RESURRECT_REQUEST", name)
end
H.__ANDROID_SPIRIT_HEALER = function(guid)
  spiritHealer = guid
  signal("CONFIRM_XP_LOSS")
end
H.__ANDROID_CORPSE_DELAY = function(ms)
  reclaimDelay = GetTime() + (tonumber(ms) or 0) / 1000
  signal("CORPSE_OUT_OF_RANGE")
end
H.__ANDROID_CORPSE = function(found, map, x, y, z)
  if found == "1" then corpse = { map = tonumber(map), x = tonumber(x), y = tonumber(y) } else corpse = nil end
end
local corpseInRange = false
local corpseQueried = 0
local function pollDeath()
  local hp = UnitHealth("player")
  local nowDead = hp == 0 and UnitHealthMax("player") > 0
  local nowGhost = isGhost()
  if nowDead and not dead and not nowGhost then
    dead = true
    deathTime = GetTime()
    signal("PLAYER_DEAD")
  end
  if nowGhost ~= ghost then
    ghost = nowGhost
    if ghost then
      dead = false
      signal("PLAYER_ALIVE")
      __AndroidSend(OP.MSG_CORPSE_QUERY, "")
    else
      corpse, corpseInRange = nil, false
      signal("PLAYER_UNGHOST")
      signal("PLAYER_ALIVE")
    end
  end
  if dead and not nowDead and not nowGhost then
    dead = false
    signal("PLAYER_ALIVE")
  end
  if ghost then
    if not corpse and GetTime() - corpseQueried > 5 then
      corpseQueried = GetTime()
      __AndroidSend(OP.MSG_CORPSE_QUERY, "")
    end
    local map, x, y = __AndroidPlayerPose()
    local near = corpse and map == corpse.map and x and ((x - corpse.x) ^ 2 + (y - corpse.y) ^ 2) < 39 * 39
    near = near and true or false
    if near ~= corpseInRange then
      corpseInRange = near
      signal(near and "CORPSE_IN_RANGE" or "CORPSE_OUT_OF_RANGE")
    end
  end
end

-- ---- Friends and ignore (SMSG_CONTACT_LIST / SMSG_FRIEND_STATUS).
local friends, ignores = {}, {}
H.__ANDROID_CONTACTS = function(count, ...)
  local v = { ... }
  local newFriends, newIgnores = {}, {}
  for k = 1, tonumber(count) or 0 do
    local b = (k - 1) * 8
    local c = { guid = v[b + 1], name = v[b + 2], flags = tonumber(v[b + 3]) or 0, note = v[b + 4],
                status = tonumber(v[b + 5]) or 0, area = tonumber(v[b + 6]) or 0, level = tonumber(v[b + 7]) or 0,
                class = tonumber(v[b + 8]) or 0 }
    if c.name == "" then c.name = "?" end
    if bit.band(c.flags, 1) ~= 0 then newFriends[#newFriends + 1] = c end
    if bit.band(c.flags, 2) ~= 0 then newIgnores[#newIgnores + 1] = c end
  end
  sort(newFriends, function(a, b)
    if (a.status > 0) ~= (b.status > 0) then return a.status > 0 end
    return a.name < b.name
  end)
  friends, ignores = newFriends, newIgnores
  signal("FRIENDLIST_UPDATE")
  signal("IGNORELIST_UPDATE")
end
local FRIEND_RESULTS = { [2] = "ERR_FRIEND_ONLINE_SS", [3] = "ERR_FRIEND_OFFLINE_S", [4] = "ERR_FRIEND_NOT_FOUND",
                         [5] = "ERR_FRIEND_REMOVED_S", [6] = "ERR_FRIEND_ADDED_S", [7] = "ERR_FRIEND_ADDED_S",
                         [8] = "ERR_FRIEND_ALREADY_S", [9] = "ERR_FRIEND_SELF", [10] = "ERR_FRIEND_ENEMY_S",
                         [12] = "ERR_IGNORE_FULL", [13] = "ERR_IGNORE_SELF", [14] = "ERR_IGNORE_NOT_FOUND",
                         [15] = "ERR_IGNORE_ALREADY_S", [16] = "ERR_IGNORE_ADDED_S", [17] = "ERR_IGNORE_REMOVED_S" }
H.__ANDROID_FRIEND_STATUS = function(result, guid, name)
  local token = FRIEND_RESULTS[tonumber(result) or -1]
  local text = token and _G[token]
  if text then
    if token == "ERR_FRIEND_ONLINE_SS" then chat(format(text, name, name)) else chat(format(text, name or "")) end
  end
  ShowFriends()
end
function ShowFriends() __AndroidSend(OP.CMSG_CONTACT_LIST, "i", 7) end
function GetNumFriends()
  local online = 0
  for _, f in ipairs(friends) do if f.status > 0 then online = online + 1 end end
  return #friends, online
end
function GetFriendInfo(i)
  local f
  if type(i) == "string" then for _, c in ipairs(friends) do if c.name == i then f = c end end
  else f = friends[tonumber(i) or 0] end
  if not f then return end
  local online = f.status > 0
  local status = f.status == 2 and CHAT_FLAG_AFK or f.status == 4 and CHAT_FLAG_DND or ""
  return f.name, online and f.level or 0, online and (className(f.class)) or "", online and areaName(f.area) or "",
         online and 1 or nil, status, f.note ~= "" and f.note or nil
end
function AddFriend(name, note) if name and name ~= "" then __AndroidSend(OP.CMSG_ADD_FRIEND, "ss", name, note or "") end end
local function findContact(list, who)
  if type(who) == "number" then return list[who] end
  for _, c in ipairs(list) do if c.name == who then return c end end
end
function RemoveFriend(who) local c = findContact(friends, who); if c then __AndroidSend(OP.CMSG_DEL_FRIEND, "l", c.guid) end end
function SetFriendNotes(who, note)
  local c = findContact(friends, who)
  if c then __AndroidSend(OP.CMSG_SET_CONTACT_NOTES, "ls", c.guid, note or ""); c.note = note or "" end
end
function GetNumIgnores() return #ignores end
function GetIgnoreName(i) local c = ignores[tonumber(i) or 0]; return c and c.name end
function AddIgnore(name) if name and name ~= "" then __AndroidSend(OP.CMSG_ADD_IGNORE, "s", name) end end
function DelIgnore(who) local c = findContact(ignores, who); if c then __AndroidSend(OP.CMSG_DEL_IGNORE, "l", c.guid) end end
function AddOrDelIgnore(name) if findContact(ignores, name) then DelIgnore(name) else AddIgnore(name) end end
function AddOrRemoveFriend(name, note) if findContact(friends, name) then RemoveFriend(name) else AddFriend(name, note) end end
local selectedFriend, selectedIgnore = 0, 0
function GetSelectedFriend() return selectedFriend end
function SetSelectedFriend(i) selectedFriend = tonumber(i) or 0 end
function GetSelectedIgnore() return selectedIgnore end
function SetSelectedIgnore(i) selectedIgnore = tonumber(i) or 0 end
function IsIgnored(name) if findContact(ignores, name) then return 1 end end

-- ---- /who (CMSG_WHO / SMSG_WHO).
local who = { shown = 0, total = 0, list = {} }
local whoToUI = false
H.__ANDROID_WHO = function(shown, total, ...)
  local v = { ... }
  who = { shown = tonumber(shown) or 0, total = tonumber(total) or 0, list = {} }
  for k = 1, who.shown do
    local b = (k - 1) * 6
    who.list[k] = { name = v[b + 1], guild = v[b + 2], level = tonumber(v[b + 3]) or 0, class = tonumber(v[b + 4]) or 0,
                    race = tonumber(v[b + 5]) or 0, zone = tonumber(v[b + 6]) or 0 }
  end
  if whoToUI or who.shown > 3 then
    signal("WHO_LIST_UPDATE")
  else
    for _, w in ipairs(who.list) do
      local race = Dbc("ChrRaces", w.race, "name") or ""
      chat(format(WHO_LIST_FORMAT or "%s - Level %d %s %s - %s", "|Hplayer:" .. w.name .. "|h[" .. w.name .. "]|h",
                  w.level, race, (className(w.class)), areaName(w.zone)))
    end
    chat(format(WHO_NUM_RESULTS or "%d player(s) total", who.total))
  end
end
function SendWho(text)
  text = text or ""
  local minLevel, maxLevel = 0, 100
  local low, high = strmatch(text, "(%d+)%s*%-%s*(%d+)")
  if low then minLevel, maxLevel = tonumber(low), tonumber(high); text = gsub(text, "%d+%s*%-%s*%d+", "") end
  local words = {}
  for word in gmatch(text, "%S+") do if #words < 4 then words[#words + 1] = word end end
  -- min, max, player name, guild name, race mask, class mask, zone count, string count, strings.
  local fmt = "iissiiii" .. strrep("s", #words)
  __AndroidSend(OP.CMSG_WHO, fmt, minLevel, maxLevel, "", "", 0xFFFFFFFF, 0xFFFFFFFF, 0, #words, unpack(words))
end
function SetWhoToUI(on) whoToUI = on and on ~= 0 end
function GetNumWhoResults() return who.shown, who.total end
function GetWhoInfo(i)
  local w = who.list[tonumber(i) or 0]
  if not w then return end
  local class, file = className(w.class)
  return w.name, w.guild, w.level, Dbc("ChrRaces", w.race, "name") or "", class, areaName(w.zone), file
end
function SortWho() signal("WHO_LIST_UPDATE") end

-- ---- Guild (PLAYER_GUILDID 0x97, PLAYER_GUILDRANK 0x98).
local guild = { name = nil, ranks = {}, motd = "", info = "", members = {} }
local guildId = 0
local showOffline = false
local function guildChanged()
  local id = Field(0x97) or 0
  if id ~= guildId then
    guildId = id
    guild = { name = nil, ranks = {}, motd = "", info = "", members = {} }
    if id > 0 then
      __AndroidSend(OP.CMSG_QUERY_GUILD_INFO, "i", id)
      __AndroidSend(OP.CMSG_GUILD_GET_ROSTER, "")
    end
    signal("PLAYER_GUILD_UPDATE", "player")
  end
end
H.__ANDROID_GUILD_INFO = function(id, name, ...)
  guild.name = name
  guild.ranks = { ... }
  signal("PLAYER_GUILD_UPDATE", "player")
  signal("GUILD_ROSTER_UPDATE")
end
H.__ANDROID_GUILD_ROSTER = function(motd, info, count, ...)
  local v = { ... }
  guild.motd, guild.info = motd, info
  local members = {}
  for k = 1, tonumber(count) or 0 do
    local b = (k - 1) * 8
    members[k] = { guid = v[b + 1], name = v[b + 2], online = v[b + 3] ~= "0", rank = tonumber(v[b + 4]) or 0,
                   level = tonumber(v[b + 5]) or 0, class = tonumber(v[b + 6]) or 0, zone = tonumber(v[b + 7]) or 0,
                   note = v[b + 8] or "" }
  end
  sort(members, function(a, b)
    if a.online ~= b.online then return a.online end
    return a.name < b.name
  end)
  guild.members = members
  signal("GUILD_ROSTER_UPDATE")
  signal("GUILD_MOTD", motd)
end
H.__ANDROID_GUILD_EVENT = function() __AndroidSend(OP.CMSG_GUILD_GET_ROSTER, "") end
function IsInGuild() if (Field(0x97) or 0) > 0 then return 1 end end
function GetGuildInfo(unit)
  if not UnitIsUnit(unit or "", "player") or not IsInGuild() then return end
  local rank = Field(0x98) or 0
  return guild.name or "", guild.ranks[rank + 1] or "", rank
end
function GuildRoster() if IsInGuild() then __AndroidSend(OP.CMSG_GUILD_GET_ROSTER, "") end end
function GetGuildRosterShowOffline() return showOffline and 1 or nil end
function SetGuildRosterShowOffline(on) showOffline = on and on ~= 0; signal("GUILD_ROSTER_UPDATE") end
local function rosterList()
  local list = {}
  for _, m in ipairs(guild.members) do if showOffline or m.online then list[#list + 1] = m end end
  return list
end
function GetNumGuildMembers()
  local online = 0
  for _, m in ipairs(guild.members) do if m.online then online = online + 1 end end
  return #guild.members, online
end
function GetGuildRosterInfo(i)
  local m = rosterList()[tonumber(i) or 0]
  if not m then return end
  local class, file = className(m.class)
  return m.name, guild.ranks[m.rank + 1] or "", m.rank, m.level, class, areaName(m.zone), m.note, "",
         m.online and 1 or nil, 0, file
end
function GetGuildRosterMOTD() return guild.motd or "" end
function GetGuildInfoText() return guild.info or "" end
function GuildControlGetNumRanks() return #guild.ranks end
function GuildControlGetRankName(i) return guild.ranks[tonumber(i) or 0] or "" end
function GetGuildRosterSelection() return 0 end
function SetGuildRosterSelection() end
function GetGuildRosterLastOnline() return 0, 0, 0, 0 end
function CanGuildInvite() return nil end
function CanEditMOTD() return nil end
function CanEditPublicNote() return nil end
function CanViewOfficerNote() return nil end
function IsGuildLeader() return nil end
function GuildInvite(name) __AndroidSend(OP.CMSG_GUILD_INVITE, "s", name or "") end
function GuildLeave() __AndroidSend(OP.CMSG_GUILD_LEAVE, "") end

-- Per-frame checks (4 times per second).
local ticker = CreateFrame("Frame")
local acc = 0
ticker:SetScript("OnUpdate", function(self, elapsed)
  acc = acc + (elapsed or 0)
  if acc < 0.25 then return end
  acc = 0
  pollDeath()
  guildChanged()
end)
function __AndroidSocialLogin()
  ShowFriends()
  guildChanged()
end

-- ---- Game time (SMSG_LOGIN_SETTIMESPEED): GetGameTime for the clock and the day/night icon.
local gameHour, gameMinute, gameAt
H.__ANDROID_GAME_TIME = function(hour, minute)
  gameHour, gameMinute, gameAt = tonumber(hour) or 0, tonumber(minute) or 0, GetTime()
end
function GetGameTime()
  if not gameAt then local t = date("*t"); return t.hour, t.min end
  local minutes = gameHour * 60 + gameMinute + floor((GetTime() - gameAt) / 60)
  return floor(minutes / 60) % 24, minutes % 60
end
function __AndroidCurrentZone() return zone, area end
