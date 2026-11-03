-- Android port: auras, casting bars, cooldowns, combat state and the combat log from the server
-- packets parsed by ServerEvents.cpp (__ANDROID_* events), in the original API shapes.
local signal = __AndroidSignal
local Dbc = __AndroidDbc
local Field = __AndroidPlayerField

-- ---- Unit tokens that currently point at a GUID (events are per unit token).
local TOKENS = { "player", "target", "focus", "pet", "npc", "mouseover", "party1", "party2", "party3", "party4",
                 "targettarget" }
function __AndroidTokensFor(guid)
  local list = {}
  if not guid then return list end
  for _, token in ipairs(TOKENS) do
    if UnitGUID(token) == guid then list[#list + 1] = token end
  end
  for i = 1, GetNumRaidMembers() do
    if UnitGUID("raid" .. i) == guid then list[#list + 1] = "raid" .. i end
  end
  return list
end
local tokensFor = __AndroidTokensFor
local function unitSignal(event, guid, ...)
  for _, token in ipairs(tokensFor(guid)) do signal(event, token, ...) end
end
local function guidOf(unit)
  if type(unit) ~= "string" then return nil end
  if strsub(unit, 1, 2) == "0x" then return unit end
  return UnitGUID(unit)
end
local function playerGuid() return __AndroidPlayerGuid() end
local function spellName(id)
  local s = __AndroidSpellData(id)
  if s then return s.name, s.rank, s.icon end
  return "Spell #" .. tostring(id), "", "Interface\\Icons\\INV_Misc_QuestionMark"
end
local SCHOOL_OF = {}
local function spellSchool(id)
  id = tonumber(id) or 0
  local school = SCHOOL_OF[id]
  if not school then
    school = Dbc("Spell", id, "schoolMask") or 1
    SCHOOL_OF[id] = school
  end
  return school
end

-- ---- Auras (SMSG_AURA_UPDATE / _ALL): auras[guid][slot].
local auras = {}
local DISPEL = { [1] = "Magic", [2] = "Curse", [3] = "Disease", [4] = "Poison" }
local auraInfo = {}
local function auraStatic(spell)
  local s = auraInfo[spell]
  if not s then
    local dispel, duration = Dbc("Spell", spell, "dispelType", "durationIndex")
    local base = duration and duration > 0 and Dbc("SpellDuration", duration, "duration") or 0
    s = { dispel = DISPEL[dispel or 0], duration = (base and base > 0) and base / 1000 or 0 }
    auraInfo[spell] = s
  end
  return s
end
local function auraList(guid, filter)
  local list = {}
  local set = auras[guid]
  if not set then return list end
  filter = strupper(filter or "HELPFUL")
  local harmful = strfind(filter, "HARMFUL") ~= nil
  local mine = strfind(filter, "PLAYER") ~= nil
  local slots = {}
  for slot in pairs(set) do slots[#slots + 1] = slot end
  sort(slots)
  local self = playerGuid()
  for _, slot in ipairs(slots) do
    local a = set[slot]
    local negative = bit.band(a.flags, 0x80) ~= 0
    if negative == harmful and (not mine or a.caster == self) then list[#list + 1] = a end
  end
  return list
end
local function auraValues(a)
  local name, rank, icon = spellName(a.spell)
  local static = auraStatic(a.spell)
  local duration = a.maximum > 0 and a.maximum / 1000 or 0
  local expires = duration > 0 and a.expires or 0
  local caster
  local self = playerGuid()
  if a.caster == self then caster = "player"
  elseif a.caster and a.caster == UnitGUID("pet") then caster = "pet"
  elseif a.caster and a.caster == UnitGUID("target") then caster = "target" end
  return name, rank, icon, a.stacks > 1 and a.stacks or 0, static.dispel, duration, expires, caster, nil, nil, a.spell
end
function UnitAura(unit, index, rank, filter)
  local guid = guidOf(unit)
  if not guid then return end
  if type(rank) == "string" and filter == nil and (strfind(strupper(rank), "HELPFUL") or strfind(strupper(rank), "HARMFUL")) then
    filter, rank = rank, nil
  end
  local list = auraList(guid, filter)
  if type(index) == "string" then
    for _, a in ipairs(list) do
      local name, r = spellName(a.spell)
      if name == index and (not rank or rank == r) then return auraValues(a) end
    end
    return
  end
  local a = list[tonumber(index) or 0]
  if a then return auraValues(a) end
end
function UnitBuff(unit, index, rank, filter) return UnitAura(unit, index, rank, "HELPFUL" .. (filter and ("|" .. filter) or "")) end
function UnitDebuff(unit, index, rank, filter) return UnitAura(unit, index, rank, "HARMFUL" .. (filter and ("|" .. filter) or "")) end
function CancelUnitBuff(unit, index, filter)
  local guid = guidOf(unit)
  if guid ~= playerGuid() then return end
  local list = auraList(guid, filter or "HELPFUL")
  local a = list[tonumber(index) or 0]
  if a then __AndroidSend(OP.CMSG_CANCEL_AURA, "i", a.spell) end
end
function GetPlayerBuff(index, filter) local a = auraList(playerGuid(), filter)[tonumber(index) or 0]; if a then return index, 0 end; return 0, 0 end
function UnitIsGhostByAura() return nil end
function IsSpellAura() return nil end
-- Combat log aura events for the player's own auras.
local function auraLog(event, a, guid)
  if not a then return end
  local name = spellName(a.spell)
  __AndroidCombatLog(event, a.caster, nil, guid, nil, a.spell, name, spellSchool(a.spell),
                     bit.band(a.flags, 0x80) ~= 0 and "DEBUFF" or "BUFF")
end

-- ---- Casting (SMSG_SPELL_START/GO/FAILURE, MSG_CHANNEL_*): casts[guid].
local casts = {}
local castId = 0
function UnitCastingInfo(unit)
  local c = casts[guidOf(unit) or ""]
  if not c or c.channel then return end
  local name, rank, icon = spellName(c.spell)
  return name, rank, name, icon, c.start * 1000, c.finish * 1000, nil, c.id, nil
end
function UnitChannelInfo(unit)
  local c = casts[guidOf(unit) or ""]
  if not c or not c.channel then return end
  local name, rank, icon = spellName(c.spell)
  return name, rank, name, icon, c.start * 1000, c.finish * 1000, nil, nil
end
local function endCast(guid, event)
  local c = casts[guid]
  if not c then return end
  casts[guid] = nil
  local name, rank = spellName(c.spell)
  if c.channel then
    unitSignal("UNIT_SPELLCAST_CHANNEL_STOP", guid, name, rank)
  else
    if event then unitSignal(event, guid, name, rank, c.id) end
    unitSignal("UNIT_SPELLCAST_STOP", guid, name, rank, c.id)
  end
end
function SpellIsTargeting() return nil end
function SpellCanTargetItem() return nil end
function SpellStopTargeting() end
function SpellTargetUnit(unit) end

-- ---- Cooldowns: cooldowns[spell] = {start, duration}; the global cooldown (category 133).
local cooldowns = {}
local gcd = { start = 0, duration = 0 }
local function recovery(spell)
  local category, start = Dbc("Spell", spell, "startRecoveryCategory", "startRecoveryTime")
  return category or 0, (start or 0) / 1000
end
local function spellCooldown(id)
  id = tonumber(id)
  if not id then return 0, 0, 1 end
  local now = GetTime()
  local c = cooldowns[id]
  local start, duration = 0, 0
  if c and c.start + c.duration > now then start, duration = c.start, c.duration end
  local category = recovery(id)
  if category == 133 and gcd.start + gcd.duration > now and gcd.start + gcd.duration > start + duration then
    start, duration = gcd.start, gcd.duration
  end
  return start, duration, 1
end
local function resolveSpellId(spell, bookType)
  if type(spell) == "number" and bookType then
    local name = GetSpellName(spell, bookType)
    if not name then return end
    spell = name
  end
  if type(spell) == "number" then return spell end
  local link = GetSpellLink(spell)
  return link and tonumber(strmatch(link, "spell:(%d+)"))
end
function GetSpellCooldown(spell, bookType) return spellCooldown(resolveSpellId(spell, bookType)) end
function GetActionCooldown(slot)
  local id, kind = __AndroidAction(tonumber(slot) or 0)
  if id and kind == 0 then return spellCooldown(id) end
  if id and kind == 128 then return GetItemCooldown(id) end
  return 0, 0, 0
end
local itemCooldowns = {}
function GetItemCooldown(item)
  local entry = __AndroidItemEntry(item)
  local spells = entry and { __AndroidItemSpells(entry) } or {}
  for i = 1, #spells, 2 do
    if spells[i] and spells[i] > 0 then
      local start, duration = spellCooldown(spells[i])
      if duration > 0 then return start, duration, 1 end
    end
  end
  return 0, 0, 1
end
function GetContainerItemCooldown(bag, slot)
  local entry = GetContainerItemID(bag, slot)
  if entry then return GetItemCooldown(entry) end
  return 0, 0, 1
end
function GetInventoryItemCooldown(unit, slot)
  local entry = GetInventoryItemID(unit, slot)
  if entry then return GetItemCooldown(entry) end
  return 0, 0, 1
end
local function cooldownsChanged()
  signal("SPELL_UPDATE_COOLDOWN")
  signal("ACTIONBAR_UPDATE_COOLDOWN")
  signal("BAG_UPDATE_COOLDOWN")
end

-- ---- Combat state: UNIT_FLAG_IN_COMBAT (0x80000) of the player's fields; melee auto attack.
local attacking, inCombat = false, false
function UnitAffectingCombat(unit)
  if UnitIsUnit(unit or "", "player") then if inCombat then return 1 end; return nil end
  local u = { __AndroidUnit(guidOf(unit) or "") }
  if u[1] and bit.band(u[15] or 0, 0x80000) ~= 0 then return 1 end
end
function IsCurrentSpell(spell, bookType)
  local id = resolveSpellId(spell, bookType)
  if id == 6603 and attacking then return 1 end
  local c = casts[playerGuid()]
  if c and c.spell == id then return 1 end
end
function IsCurrentAction(slot)
  local id, kind = __AndroidAction(tonumber(slot) or 0)
  if id and kind == 0 then return IsCurrentSpell(id) end
end
function IsAutoRepeatAction() return nil end
local function pollCombat()
  local flags = Field(0x3B) or 0
  local now = bit.band(flags, 0x80000) ~= 0
  if now ~= inCombat then
    inCombat = now
    signal(now and "PLAYER_REGEN_DISABLED" or "PLAYER_REGEN_ENABLED")
    unitSignal("UNIT_FLAGS", playerGuid())
  end
end

-- ---- Combat log: COMBAT_LOG_EVENT(_UNFILTERED) with 3.3.5 arguments.
local function unitFlags(guid)
  if not guid or guid == "0x0000000000000000" then return 0 end
  if guid == playerGuid() then return 0x511 end
  local u = { __AndroidUnit(guid) }
  if not u[1] then
    -- Players have the high GUID 0x0000, creatures 0xF130, pets 0xF140.
    if strsub(guid, 3, 6) == "0000" then return 0x548 end
    return 0xA48
  end
  local isPlayer, npcFlags = u[13], u[16] or 0
  local flags = isPlayer and 0x400 or 0x800
  flags = flags + (isPlayer and 0x100 or 0x200)
  if UnitInParty(guid) then flags = flags + 0x2 + 0x10
  elseif isPlayer or npcFlags ~= 0 then flags = flags + 0x10 + 0x8
  else flags = flags + 0x40 + 0x8 end
  if guid == UnitGUID("target") then flags = flags + 0x10000 end
  return flags
end
local function nameOf(guid, name)
  if name and name ~= "" then return name end
  if not guid then return nil end
  local u = { __AndroidUnit(guid) }
  if u[1] then return u[3] end
end
function __AndroidCombatLog(event, src, srcName, dst, dstName, ...)
  local timestamp = time()
  signal("COMBAT_LOG_EVENT_UNFILTERED", timestamp, event, src, nameOf(src, srcName), unitFlags(src), dst,
         nameOf(dst, dstName), unitFlags(dst), ...)
  signal("COMBAT_LOG_EVENT", timestamp, event, src, nameOf(src, srcName), unitFlags(src), dst,
         nameOf(dst, dstName), unitFlags(dst), ...)
end
local log = __AndroidCombatLog
local MISS = { [1] = "MISS", [2] = "RESIST", [3] = "DODGE", [4] = "PARRY", [5] = "BLOCK", [6] = "EVADE",
               [7] = "IMMUNE", [8] = "IMMUNE", [9] = "DEFLECT", [10] = "ABSORB", [11] = "REFLECT" }
local VICTIM = { [2] = "DODGE", [3] = "PARRY", [4] = "INTERRUPT", [5] = "BLOCK", [6] = "EVADE", [7] = "IMMUNE",
                 [8] = "DEFLECT" }
local function feedback(guid, action, descriptor, amount, school)
  -- UNIT_COMBAT drives the hit numbers on the player and target portraits.
  unitSignal("UNIT_COMBAT", guid, action, descriptor or "", amount or 0, school or 1)
end

-- ---- Chat lines the original builds from the same packets.
local function chat(event, text) signal(event, text, "", "", "", "", "", 0, 0, "", 0, 0, "") end
local pendingLines = {}
local function itemLine(event, pattern, entry, count, who)
  pendingLines[#pendingLines + 1] = { event = event, pattern = pattern, entry = entry, count = count, who = who,
                                      deadline = GetTime() + 3 }
end
local function flushLines()
  local keep = {}
  for _, line in ipairs(pendingLines) do
    local _, link = GetItemInfo(line.entry)
    if link or GetTime() > line.deadline then
      link = link or ("[item:" .. line.entry .. "]")
      local text
      if line.who then text = format(line.pattern, line.who, link) else text = format(line.pattern, link) end
      if line.count and line.count > 1 then text = gsub(text, "%.$", "") .. "x" .. line.count .. "." end
      chat(line.event, text)
    else
      keep[#keep + 1] = line
    end
  end
  pendingLines = keep
end

-- ---- Event handlers.
local H = {}
__AndroidCombatHandlers = H
H.__ANDROID_AURA_CLEAR = function(guid)
  auras[guid] = {}
  unitSignal("UNIT_AURA", guid)
end
H.__ANDROID_AURA = function(guid, slot, spell, flags, level, stacks, maximum, remaining, caster)
  slot, spell = tonumber(slot), tonumber(spell)
  auras[guid] = auras[guid] or {}
  local old = auras[guid][slot]
  if spell == 0 then
    auras[guid][slot] = nil
    auraLog("SPELL_AURA_REMOVED", old, guid)
  else
    local a = { spell = spell, flags = tonumber(flags) or 0, level = tonumber(level) or 0, stacks = tonumber(stacks) or 0,
                maximum = tonumber(maximum) or 0, caster = caster }
    remaining = tonumber(remaining) or 0
    if bit.band(a.flags, 0x20) == 0 and old and old.spell == spell then
      a.maximum, a.expires = old.maximum, old.expires
    else
      a.expires = GetTime() + remaining / 1000
    end
    auras[guid][slot] = a
    if not old or old.spell ~= spell then auraLog("SPELL_AURA_APPLIED", a, guid) end
  end
  unitSignal("UNIT_AURA", guid)
  if guid == playerGuid() then signal("PLAYER_AURAS_CHANGED") end
end
H.__ANDROID_CAST_START = function(guid, spell, castTime)
  spell, castTime = tonumber(spell), tonumber(castTime) or 0
  local name, rank = spellName(spell)
  log("SPELL_CAST_START", guid, nil, nil, nil, spell, name, spellSchool(spell))
  if guid == playerGuid() then
    local category, start = recovery(spell)
    if category == 133 and start > 0 then gcd = { start = GetTime(), duration = start }; cooldownsChanged() end
  end
  if castTime <= 0 then return end
  castId = castId + 1
  local now = GetTime()
  casts[guid] = { spell = spell, start = now, finish = now + castTime / 1000, id = castId }
  unitSignal("UNIT_SPELLCAST_START", guid, name, rank, castId)
end
H.__ANDROID_CAST_GO = function(guid, spell, casterName)
  spell = tonumber(spell)
  local name, rank = spellName(spell)
  log("SPELL_CAST_SUCCESS", guid, casterName, nil, nil, spell, name, spellSchool(spell))
  local c = casts[guid]
  if c and not c.channel and c.spell == spell then
    unitSignal("UNIT_SPELLCAST_SUCCEEDED", guid, name, rank, c.id)
    endCast(guid)
  elseif not c then
    unitSignal("UNIT_SPELLCAST_SUCCEEDED", guid, name, rank, 0)
  end
  if guid == playerGuid() then
    local category, start = recovery(spell)
    if category == 133 and start > 0 and gcd.start + gcd.duration < GetTime() then
      gcd = { start = GetTime(), duration = start }
    end
    cooldownsChanged()
  end
end
H.__ANDROID_CAST_INTERRUPT = function(guid, spell, reason)
  local c = casts[guid]
  if c and c.spell == tonumber(spell) then endCast(guid, "UNIT_SPELLCAST_INTERRUPTED") end
end
H.__ANDROID_CAST_FAILED = function(spell, result)
  local guid = playerGuid()
  local c = casts[guid]
  if c and not c.channel then endCast(guid, "UNIT_SPELLCAST_FAILED")
  else
    local name, rank = spellName(tonumber(spell))
    signal("UNIT_SPELLCAST_FAILED", "player", name, rank, 0)
  end
  if gcd.start + gcd.duration > GetTime() then gcd = { start = 0, duration = 0 }; cooldownsChanged() end
  local token = SPELL_FAILED_TOKENS and SPELL_FAILED_TOKENS[tonumber(result) or -1]
  local text = token and _G[token]
  if text and text ~= "" then signal("UI_ERROR_MESSAGE", text) end
end
H.__ANDROID_CHANNEL_START = function(guid, spell, duration)
  spell, duration = tonumber(spell), tonumber(duration) or 0
  local now = GetTime()
  casts[guid] = { spell = spell, start = now, finish = now + duration / 1000, channel = true }
  local name, rank = spellName(spell)
  unitSignal("UNIT_SPELLCAST_CHANNEL_START", guid, name, rank)
end
H.__ANDROID_CHANNEL_UPDATE = function(guid, remaining)
  remaining = tonumber(remaining) or 0
  local c = casts[guid]
  if not c or not c.channel then return end
  if remaining <= 0 then endCast(guid); return end
  c.finish = GetTime() + remaining / 1000
  local name, rank = spellName(c.spell)
  unitSignal("UNIT_SPELLCAST_CHANNEL_UPDATE", guid, name, rank)
end
H.__ANDROID_COOLDOWN = function(spell, ms)
  spell, ms = tonumber(spell), tonumber(ms) or 0
  if ms <= 0 then
    -- Cooldown starts when the event arrives (SMSG_COOLDOWN_EVENT) or was cleared.
    cooldowns[spell] = nil
  else
    cooldowns[spell] = { start = GetTime(), duration = ms / 1000 }
  end
  cooldownsChanged()
end
H.__ANDROID_COOLDOWN_EVENT = function(spell)
  spell = tonumber(spell)
  local recoveryTime, categoryTime = Dbc("Spell", spell, "recoveryTime", "categoryRecoveryTime")
  local ms = math.max(recoveryTime or 0, categoryTime or 0)
  if ms > 0 then cooldowns[spell] = { start = GetTime(), duration = ms / 1000 } end
  cooldownsChanged()
end
H.__ANDROID_ATTACK_START = function(attacker, victim)
  if attacker == playerGuid() then
    attacking = true
    signal("PLAYER_ENTER_COMBAT")
    signal("ACTIONBAR_UPDATE_STATE")
  end
end
H.__ANDROID_ATTACK_STOP = function(attacker, victim)
  if attacker == playerGuid() then
    attacking = false
    signal("PLAYER_LEAVE_COMBAT")
    signal("ACTIONBAR_UPDATE_STATE")
  end
end
H.__ANDROID_SWING = function(src, srcName, dst, dstName, damage, overkill, school, absorbed, resisted, hit, victimState)
  damage, hit, victimState = tonumber(damage) or 0, tonumber(hit) or 0, tonumber(victimState) or 1
  local crit = bit.band(hit, 0x200) ~= 0 and 1 or nil
  local glancing = bit.band(hit, 0x4000) ~= 0 and 1 or nil
  local crushing = bit.band(hit, 0x8000) ~= 0 and 1 or nil
  local miss = bit.band(hit, 0x10) ~= 0 and "MISS" or VICTIM[victimState]
  if miss and damage == 0 then
    log("SWING_MISSED", src, srcName, dst, dstName, miss)
    feedback(dst, miss)
    return
  end
  log("SWING_DAMAGE", src, srcName, dst, dstName, damage, tonumber(overkill) or 0, tonumber(school) or 1,
      tonumber(resisted) or 0, 0, tonumber(absorbed) or 0, crit, glancing, crushing)
  feedback(dst, "WOUND", crit and "CRITICAL" or (glancing and "GLANCING") or (crushing and "CRUSHING") or "", damage,
           tonumber(school) or 1)
end
H.__ANDROID_SPELL_DAMAGE = function(src, srcName, dst, dstName, spell, damage, overkill, school, absorbed, resisted, blocked, crit)
  spell = tonumber(spell)
  log("SPELL_DAMAGE", src, srcName, dst, dstName, spell, (spellName(spell)), spellSchool(spell), tonumber(damage) or 0,
      tonumber(overkill) or 0, tonumber(school) or 1, tonumber(resisted) or 0, tonumber(blocked) or 0,
      tonumber(absorbed) or 0, crit == "1" and 1 or nil, nil, nil)
  feedback(dst, "WOUND", crit == "1" and "CRITICAL" or "", tonumber(damage) or 0, tonumber(school) or 1)
end
H.__ANDROID_SPELL_HEAL = function(src, srcName, dst, dstName, spell, heal, overheal, absorbed, crit)
  spell = tonumber(spell)
  log("SPELL_HEAL", src, srcName, dst, dstName, spell, (spellName(spell)), spellSchool(spell), tonumber(heal) or 0,
      tonumber(overheal) or 0, tonumber(absorbed) or 0, crit ~= "0" and 1 or nil)
  feedback(dst, "HEAL", crit ~= "0" and "CRITICAL" or "", tonumber(heal) or 0, spellSchool(spell))
end
H.__ANDROID_PERIODIC_DAMAGE = function(src, srcName, dst, dstName, spell, damage, overkill, school, absorbed, resisted, crit)
  spell = tonumber(spell)
  log("SPELL_PERIODIC_DAMAGE", src, srcName, dst, dstName, spell, (spellName(spell)), spellSchool(spell),
      tonumber(damage) or 0, tonumber(overkill) or 0, tonumber(school) or 1, tonumber(resisted) or 0, 0,
      tonumber(absorbed) or 0, crit ~= "0" and 1 or nil, nil, nil)
  feedback(dst, "WOUND", "", tonumber(damage) or 0, tonumber(school) or 1)
end
H.__ANDROID_PERIODIC_HEAL = function(src, srcName, dst, dstName, spell, heal, overheal, absorbed, crit)
  spell = tonumber(spell)
  log("SPELL_PERIODIC_HEAL", src, srcName, dst, dstName, spell, (spellName(spell)), spellSchool(spell),
      tonumber(heal) or 0, tonumber(overheal) or 0, tonumber(absorbed) or 0, crit ~= "0" and 1 or nil)
  feedback(dst, "HEAL", "", tonumber(heal) or 0, spellSchool(spell))
end
H.__ANDROID_PERIODIC_ENERGIZE = function(src, srcName, dst, dstName, spell, amount, power)
  spell = tonumber(spell)
  log("SPELL_PERIODIC_ENERGIZE", src, srcName, dst, dstName, spell, (spellName(spell)), spellSchool(spell),
      tonumber(amount) or 0, tonumber(power) or 0)
  feedback(dst, "ENERGIZE", "", tonumber(amount) or 0, spellSchool(spell))
end
H.__ANDROID_SPELL_MISS = function(src, srcName, dst, dstName, spell, miss)
  spell = tonumber(spell)
  local kind = MISS[tonumber(miss) or 1] or "MISS"
  log("SPELL_MISSED", src, srcName, dst, dstName, spell, (spellName(spell)), spellSchool(spell), kind)
  feedback(dst, kind)
end
H.__ANDROID_XP_GAIN = function(victim, victimName, xp, kind)
  xp = tonumber(xp) or 0
  if kind == "0" and victimName and victimName ~= "" then
    chat("CHAT_MSG_COMBAT_XP_GAIN", format(COMBATLOG_XPGAIN_FIRSTPERSON or "%s dies, you gain %d experience.", victimName, xp))
    log("UNIT_DIED", nil, nil, victim, victimName)
  else
    chat("CHAT_MSG_COMBAT_XP_GAIN", format(COMBATLOG_XPGAIN_FIRSTPERSON_UNNAMED or "You gain %d experience.", xp))
  end
end
local lastLevel
H.__ANDROID_LEVEL_UP = function(level, hp, ...)
  local v = { ... }
  local powers = { tonumber(v[1]) or 0 }
  local stats = {}
  for i = 7, 11 do stats[#stats + 1] = tonumber(v[i]) or 0 end
  local talents = (tonumber(level) or 0) >= 10 and 1 or 0
  signal("PLAYER_LEVEL_UP", tonumber(level) or 0, tonumber(hp) or 0, powers[1], talents, stats[1] or 0, stats[2] or 0,
         stats[3] or 0, stats[4] or 0, stats[5] or 0)
  chat("CHAT_MSG_SYSTEM", format(LEVEL_UP or "Congratulations, you have reached level %d!", tonumber(level) or 0))
end
H.__ANDROID_ITEM_PUSH = function(guid, name, entry, count, received, created)
  entry, count = tonumber(entry), tonumber(count) or 1
  GetItemInfo(entry) -- asks for the template
  if guid == playerGuid() then
    local pattern = created == "1" and (LOOT_ITEM_CREATED_SELF or "You create: %s.")
                 or received == "1" and (LOOT_ITEM_PUSHED_SELF or "You receive item: %s.")
                 or (LOOT_ITEM_SELF or "You receive loot: %s.")
    itemLine("CHAT_MSG_LOOT", pattern, entry, count)
  else
    itemLine("CHAT_MSG_LOOT", LOOT_ITEM or "%s receives loot: %s.", entry, count, name)
  end
end
H.__ANDROID_LOOT_MONEY = function(amount)
  chat("CHAT_MSG_MONEY", format(YOU_LOOT_MONEY or "You loot %s", GetCoinText(tonumber(amount) or 0, ", ")))
end
H.__ANDROID_EMOTE = function(guid, anim) end
H.__ANDROID_PLAY_SOUND = function(id) PlaySound(tonumber(id)) end
H.__ANDROID_INVENTORY_ERROR = function(code)
  local errors = { [1] = ERR_ITEM_CANT_BE_EQUIPPED, [2] = ERR_CANT_EQUIP_LEVEL_I, [8] = ERR_ITEM_DOESNT_GO_TO_SLOT,
                   [9] = ERR_ITEM_MAX_COUNT, [21] = ERR_BAG_FULL, [22] = ERR_ITEM_MAX_COUNT, [25] = ERR_INV_FULL,
                   [29] = ERR_ITEM_LOCKED, [50] = ERR_NOT_EQUIPPABLE }
  local text = errors[tonumber(code) or 0]
  if text then signal("UI_ERROR_MESSAGE", text) end
end

-- Per-frame upkeep: casts that ended without a packet, aura expiry, combat flag, pending lines.
local ticker = CreateFrame("Frame")
local elapsedTotal = 0
ticker:SetScript("OnUpdate", function(self, elapsed)
  elapsedTotal = elapsedTotal + (elapsed or 0)
  if elapsedTotal < 0.1 then return end
  elapsedTotal = 0
  local now = GetTime()
  for guid, c in pairs(casts) do
    if now > c.finish + 0.5 then endCast(guid) end
  end
  for guid, set in pairs(auras) do
    local dirty = false
    for slot, a in pairs(set) do
      if a.maximum > 0 and a.expires + 1 < now then set[slot] = nil; dirty = true end
    end
    if dirty then unitSignal("UNIT_AURA", guid) end
  end
  pollCombat()
  if #pendingLines > 0 then flushLines() end
end)
-- Forget units that left the screen except the player (their auras arrive again on sight).
function __AndroidCombatReset()
  auras, casts, cooldowns = {}, {}, {}
  gcd = { start = 0, duration = 0 }
end
