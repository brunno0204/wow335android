-- Android port: in-game Lua API subset backed by the native world state, executed before
-- Interface\FrameXML\FrameXML.toc. Every other original API function (list generated from
-- the decompiled client) becomes a stub so FrameXML loads; GetNum* stubs return 0.
local Unit = __AndroidUnit -- (unit) -> exists, guid, name, level, hp, maxhp, power, maxpower,
                           --    powertype, race, class, sex, isplayer, dead, flags, npcflags

local RACES = {
  [1] = {"Human", "Human"}, [2] = {"Orc", "Orc"}, [3] = {"Dwarf", "Dwarf"},
  [4] = {"Night Elf", "NightElf"}, [5] = {"Undead", "Scourge"}, [6] = {"Tauren", "Tauren"},
  [7] = {"Gnome", "Gnome"}, [8] = {"Troll", "Troll"}, [10] = {"Blood Elf", "BloodElf"},
  [11] = {"Draenei", "Draenei"},
}
local CLASSES = {
  [1] = {"Warrior", "WARRIOR"}, [2] = {"Paladin", "PALADIN"}, [3] = {"Hunter", "HUNTER"},
  [4] = {"Rogue", "ROGUE"}, [5] = {"Priest", "PRIEST"}, [6] = {"Death Knight", "DEATHKNIGHT"},
  [7] = {"Shaman", "SHAMAN"}, [8] = {"Mage", "MAGE"}, [9] = {"Warlock", "WARLOCK"},
  [11] = {"Druid", "DRUID"},
}
local POWER_TOKENS = { [0] = "MANA", [1] = "RAGE", [2] = "FOCUS", [3] = "ENERGY", [4] = "HAPPINESS",
                       [5] = "RUNES", [6] = "RUNIC_POWER" }
local HORDE = { [2] = true, [5] = true, [6] = true, [8] = true, [10] = true }

local function info(unit)
  if type(unit) ~= "string" then return nil end
  local token = strlower(unit)
  -- Unit tokens beyond player/target (party1, raid3, targettarget...) resolve to GUIDs (44_party.lua).
  if __AndroidUnitToken then token = __AndroidUnitToken(token) or token end
  local exists, guid, name, level, hp, maxhp, power, maxpower, ptype, race, class, sex, isplayer,
        dead, flags, npcflags, reaction = Unit(token)
  if not exists then
    if __AndroidUnitFallback then return __AndroidUnitFallback(token) end
    return nil
  end
  return { guid = guid, name = name, level = level, hp = hp, maxhp = maxhp, power = power,
           maxpower = maxpower, ptype = ptype, race = race, class = class, sex = sex,
           isplayer = isplayer, dead = dead, flags = flags, npcflags = npcflags, reaction = reaction }
end

function GetTime() return __AndroidTime() end
function GetFramerate() return __AndroidFramerate() end
function GetScreenWidth() local w = __AndroidScreen(); return w end
function GetScreenHeight() local _, h = __AndroidScreen(); return h end
function GetScreenResolutions() local w, h = __AndroidPixels(); return w .. "x" .. h end
function GetCurrentResolution() return 1 end
function GetLocale() return __AndroidLocale() end
function GetBuildInfo() return "3.3.5", "12340", "Jun 24 2010", 30300 end
function IsLoggedIn() return 1 end
function InCombatLockdown() return nil end
function IsMacClient() return nil end
function IsLinuxClient() return nil end
function IsWindowsClient() return 1 end
function GetRealmName() return __AndroidRealm() end

-- CVars: the client's registered defaults (000_cvars.lua), with a few port settings.
local cvarDefaults = __AndroidCVarDefaults or {}
local cvars = {}
for k, v in pairs(cvarDefaults) do cvars[strlower(k)] = v end
local OVERRIDES = {
  Sound_EnableAllSound = "1", showTutorials = "0", showNewbieTips = "0", showGameTips = "1",
  lockActionBars = "1", screenshotFormat = "jpeg", ShowClassColorInNameplate = "0",
}
for k, v in pairs(OVERRIDES) do cvars[strlower(k)] = v end
cvars.locale = __AndroidLocale()
cvars.realmname = __AndroidRealm()
local function key(name) return type(name) == "string" and strlower(name) end
-- Unknown names: the original registers many more (engine, sound, video); "0" as their value.
function GetCVar(name) local k = key(name); if not k then return end; local v = cvars[k]; if v == nil then return "0" end; return v end
function GetCVarDefault(name) local k = key(name); if not k then return end
  for n, v in pairs(cvarDefaults) do if strlower(n) == k then return v end end
  return GetCVar(name)
end
function GetCVarBool(name) local v = GetCVar(name); if v and v ~= "0" and v ~= "" then return 1 end; return nil end
function SetCVar(name, value) local k = key(name); if k then cvars[k] = tostring(value or "") end end
function RegisterCVar(name, value) local k = key(name); if k and cvars[k] == nil then cvars[k] = tostring(value or "") end end
function GetCVarInfo(name) local v = GetCVar(name); if v then return v, GetCVarDefault(name) end end
function GetCVarMin() return 0 end
function GetCVarMax() return 1 end
function GetCVarAbsoluteMin() return 0 end
function GetCVarAbsoluteMax() return 1 end

-- Units.
function UnitExists(unit) if info(unit) then return 1 end; return nil end
function UnitGUID(unit) local u = info(unit); return u and u.guid end
function UnitName(unit) local u = info(unit); if u then return u.name, nil end; return nil end
function UnitPVPName(unit) return UnitName(unit) end
function UnitLevel(unit) local u = info(unit); return u and u.level or 0 end
function UnitEffectiveLevel(unit) return UnitLevel(unit) end
function UnitHealth(unit) local u = info(unit); return u and u.hp or 0 end
function UnitHealthMax(unit) local u = info(unit); return u and u.maxhp or 0 end
function UnitPowerType(unit)
  local u = info(unit); local t = u and u.ptype or 0
  return t, POWER_TOKENS[t] or "MANA"
end
function UnitPower(unit, kind)
  local u = info(unit); if not u then return 0 end
  if kind == nil or kind == u.ptype then return u.power end
  return 0
end
function UnitPowerMax(unit, kind)
  local u = info(unit); if not u then return 0 end
  if kind == nil or kind == u.ptype then return u.maxpower end
  return 0
end
function UnitMana(unit) return UnitPower(unit) end
function UnitManaMax(unit) return UnitPowerMax(unit) end
function UnitIsPlayer(unit) local u = info(unit); if u and u.isplayer then return 1 end end
function UnitPlayerControlled(unit) return UnitIsPlayer(unit) end
function UnitIsDead(unit) local u = info(unit); if u and u.dead then return 1 end end
function UnitIsGhost() return nil end
function UnitIsDeadOrGhost(unit) return UnitIsDead(unit) end
function UnitIsConnected(unit) if info(unit) then return 1 end end
function UnitIsUnit(a, b) local x, y = info(a), info(b); if x and y and x.guid == y.guid then return 1 end end
function UnitIsVisible(unit) return UnitExists(unit) end
function UnitInRange(unit) return UnitExists(unit) end
function UnitIsTapped() return nil end
function UnitIsTappedByPlayer() return nil end
function UnitIsPartyLeader() return nil end
function UnitIsPVP() return nil end
function UnitIsPVPFreeForAll() return nil end
function UnitIsPVPSanctuary() return nil end
function UnitAffectingCombat() return nil end
function UnitIsAFK() return nil end
function UnitIsDND() return nil end
function UnitIsCorpse() return nil end
function UnitIsCharmed() return nil end
function UnitIsPossessed() return nil end
function UnitIsTrivial() return nil end
function UnitInParty() return nil end
function UnitInRaid() return nil end
function UnitHasVehicleUI() return nil end
function UnitUsingVehicle() return nil end
function UnitInVehicle() return nil end
function UnitHasRelicSlot() return nil end
function UnitCastingInfo() return nil end
function UnitChannelInfo() return nil end
function UnitBuff() return nil end
function UnitDebuff() return nil end
function UnitAura() return nil end
function UnitThreatSituation() return nil end
function UnitDetailedThreatSituation() return nil end
function IsResting() return nil end
function UnitSex(unit) local u = info(unit); return u and (u.sex == 1 and 3 or 2) or 1 end
function UnitRace(unit)
  local u = info(unit); local r = u and RACES[u.race]
  if r then return r[1], r[2] end
end
function UnitClass(unit)
  local u = info(unit); local c = u and CLASSES[u.class]
  if c then return c[1], c[2], u.class end
  if u then return UnitName(unit), "WARRIOR", 1 end
end
function UnitClassBase(unit) local _, token = UnitClass(unit); return token end
function UnitFactionGroup(unit)
  local u = info(unit)
  if u and u.isplayer then
    if HORDE[u.race] then return "Horde", "Horde" end
    return "Alliance", "Alliance"
  end
end
function UnitReaction(a, b)
  local u = info(b) or info(a)
  if not u then return nil end
  if u.reaction then return u.reaction end
  if u.isplayer then return 5 end
  if u.npcflags and u.npcflags ~= 0 then return 5 end
  return 4
end
function UnitIsFriend(a, b) local r = UnitReaction(a, b); if r and r >= 5 then return 1 end end
function UnitIsEnemy(a, b) local r = UnitReaction(a, b); if r and r <= 2 then return 1 end end
function UnitCanAttack(a, b) local u = info(b); if u and u.reaction and u.guid ~= UnitGUID("player") then if u.reaction <= 3 and bit.band(u.flags or 0, 0x2) == 0 then return 1 end; return nil end; if u and not u.isplayer and (u.npcflags or 0) == 0 then return 1 end end
function UnitCanAssist(a, b) return UnitIsFriend(a, b) end
function UnitCanCooperate(a, b) return UnitIsFriend(a, b) end
function UnitClassification(unit) if info(unit) then return "normal" end end
function UnitCreatureType() return nil end
function UnitCreatureFamily() return nil end
function UnitSelectionColor(unit)
  -- CGGameUI::GetSelectionHighlightColor reaction colors.
  local u = info(unit)
  if u and u.dead and not u.isplayer then return .5, .5, .5, 1 end
  local r = UnitReaction(unit, "player") or 3
  if r >= 4 then return 0, 1, 0, 1 elseif r == 3 then return 1, 1, 0, 1 elseif r == 2 then return 1, .5, 0, 1 end
  return 1, 0, 0, 1
end
function GetUnitName(unit, showServer) return (UnitName(unit)) end
function TargetUnit(unit) local u = info(unit); __AndroidTarget(u and u.guid) end
function ClearTarget() __AndroidTarget(nil) end

-- Portraits: the class icon until the 3D portrait of the unit's model is rendered.
local CLASS_ICON = {
  WARRIOR = {0, .25, 0, .25}, MAGE = {.25, .49609375, 0, .25}, ROGUE = {.49609375, .7421875, 0, .25},
  DRUID = {.7421875, .98828125, 0, .25}, HUNTER = {0, .25, .25, .5}, SHAMAN = {.25, .49609375, .25, .5},
  PRIEST = {.49609375, .7421875, .25, .5}, WARLOCK = {.7421875, .98828125, .25, .5},
  PALADIN = {0, .25, .5, .75}, DEATHKNIGHT = {.25, .49609375, .5, .75},
}
function SetPortraitTexture(texture, unit)
  if not texture then return end
  local _, token = UnitClass(unit)
  local c = token and CLASS_ICON[token]
  if UnitIsPlayer(unit) and c then
    texture:SetTexture("Interface\\TargetingFrame\\UI-Classes-Circles")
    texture:SetTexCoord(c[1], c[2], c[3], c[4])
  else
    texture:SetTexture("Interface\\CharacterFrame\\TempPortrait")
    texture:SetTexCoord(0, 1, 0, 1)
  end
  -- 3D portrait of the loaded model (UiModelFrames.cpp) replaces the icon once rendered.
  if __AndroidPortraitRequest then __AndroidPortraitRequest(texture, UnitGUID(unit), unit) end
end
function SetPortraitToTexture(texture, path) if texture and texture.SetTexture then texture:SetTexture(path) end end

-- World/zone.
function GetZoneText() return __AndroidZone() end
function GetRealZoneText() return __AndroidZone() end
function GetMinimapZoneText() return __AndroidZone() end
function GetSubZoneText() return "" end
function GetZonePVPInfo() return nil end
function GetGameTime() local t = date("*t"); return t.hour, t.min end
function GetPlayerMapPosition() return 0, 0 end
function IsInInstance() return nil, "none" end
function GetInstanceInfo() return GetZoneText(), "none", 0, "", 5 end

-- Shapeshift/pet bars: no forms or pets yet (40_items, 41_spells, 42_actions hold the rest).
function GetNumShapeshiftForms() return 0 end
function GetShapeshiftFormInfo() return nil end
function GetPetActionInfo() return nil end
function GetBindingKey() return nil end
function GetBindingText(key) return key or "" end
function GetBindingAction() return "" end


-- Script errors always reach the Android log: FrameXML's own handler (BasicControls, the
-- error dialog) is remembered but not installed.
do
  local install = seterrorhandler
  install(__AndroidErrorHandler)
  local scriptHandler
  function seterrorhandler(handler) scriptHandler = handler end
  function geterrorhandler() return scriptHandler or __AndroidErrorHandler end
end
