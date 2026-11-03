-- Android port: group from SMSG_GROUP_LIST; party/raid unit tokens resolve to member GUIDs.
local party = {}        -- members other than the player: {name, guid, online, subGroup, flags}
local leader, groupType, lootMethod, lootThreshold = nil, 0, 0, 2
local LOOT_METHODS = { [0] = "freeforall", [1] = "roundrobin", [2] = "master", [3] = "group", [4] = "needbeforegreed" }

local function rebuild()
  party = {}
  local values = { __AndroidParty() }
  leader, groupType, lootMethod, lootThreshold = values[1], values[2] or 0, values[3] or 0, values[4] or 2
  local self = __AndroidPlayerGuid()
  for i = 5, #values, 5 do
    local m = { name = values[i], guid = values[i + 1], online = values[i + 2] ~= 0, subGroup = values[i + 3], flags = values[i + 4] }
    if m.guid ~= self then party[#party + 1] = m end
  end
end
function __AndroidPartyChanged() rebuild() end

local function isRaid() return bit.band(groupType or 0, 2) ~= 0 end
local function member(token)
  local n = tonumber(strmatch(token, "^party(%d)$") or "")
  if n then return not isRaid() and party[n] or (isRaid() and party[n]) end
  n = tonumber(strmatch(token, "^raid(%d+)$") or "")
  if n and isRaid() then
    if n == #party + 1 then return { guid = __AndroidPlayerGuid(), self = true } end
    return party[n]
  end
end
function __AndroidUnitToken(token)
  local m = member(token)
  if m then return m.guid end
end
-- Members outside visibility range: name and online state only.
function __AndroidUnitFallback(token)
  for _, m in ipairs(party) do
    if m.guid == token then
      return { guid = m.guid, name = m.name, level = 0, hp = m.online and 1 or 0, maxhp = 1, power = 0, maxpower = 0,
               ptype = 0, race = 0, class = 0, sex = 0, isplayer = true, dead = false, flags = 0, npcflags = 0,
               far = true, online = m.online }
    end
  end
end

function GetNumPartyMembers() if #party > 0 and not isRaid() then return #party end; return 0 end
function GetNumRaidMembers() if isRaid() then return #party + 1 end; return 0 end
function GetPartyMember(index) local m = party[tonumber(index) or 0]; if m and not isRaid() then return 1 end end
function UnitInParty(unit)
  if #party == 0 then return nil end
  if UnitIsUnit(unit, "player") then return 1 end
  local guid = UnitGUID(unit)
  for _, m in ipairs(party) do if m.guid == guid then return 1 end end
end
function UnitInRaid(unit) if isRaid() and UnitInParty(unit) then return 1 end end
function IsPartyLeader() if leader and leader == __AndroidPlayerGuid() and #party > 0 then return 1 end end
function IsRaidLeader() if isRaid() then return IsPartyLeader() end end
function IsRaidOfficer() return nil end
function UnitIsPartyLeader(unit) local guid = UnitGUID(unit); if guid and guid == leader then return 1 end end
function UnitIsRaidOfficer() return nil end
function GetPartyLeaderIndex()
  for i, m in ipairs(party) do if m.guid == leader then return i end end
  return 0
end
function GetRaidRosterInfo(index)
  if not isRaid() then return end
  local m = party[tonumber(index) or 0]
  if m then return m.name, 0, m.subGroup + 1, 0, nil, nil, nil, m.online and 1 or nil, nil, nil, nil end
  if tonumber(index) == #party + 1 then
    local class, file = UnitClass("player")
    return UnitName("player"), 0, 1, UnitLevel("player"), class, file, GetZoneText(), 1, nil, nil, nil
  end
end
function GetLootMethod()
  local method = LOOT_METHODS[lootMethod] or "freeforall"
  return method, nil, nil
end
function GetLootThreshold() return lootThreshold or 2 end
function UnitIsConnected(unit)
  if UnitIsUnit(unit, "player") then return 1 end
  local guid = UnitGUID(unit)
  for _, m in ipairs(party) do if m.guid == guid then return m.online and 1 or nil end end
  if UnitExists(unit) then return 1 end
end

function InviteUnit(name) if name and name ~= "" then __AndroidSend(OP.CMSG_GROUP_INVITE, "si", name, 0) end end
InviteByName = InviteUnit
function UninviteUnit(name) if name then __AndroidSend(OP.CMSG_GROUP_UNINVITE, "s", name) end end
function AcceptGroup() __AndroidSend(OP.CMSG_GROUP_ACCEPT, "i", 0) end
function DeclineGroup() __AndroidSend(OP.CMSG_GROUP_DECLINE, "") end
function LeaveParty() __AndroidSend(OP.CMSG_GROUP_DISBAND, "") end
function PromoteToLeader(unit)
  local guid = UnitGUID(unit)
  if not guid then for _, m in ipairs(party) do if m.name == unit then guid = m.guid end end end
  if guid then __AndroidSend(OP.CMSG_GROUP_SET_LEADER, "l", guid) end
end
function ConvertToRaid() __AndroidSend(OP.CMSG_GROUP_RAID_CONVERT, "") end
