-- Android port: spellbook from the known spells (SMSG_SEND_KNOWN_SPELLS / LEARNED / REMOVED)
-- and Spell, SpellIcon, SkillLine, SkillLineAbility; casting with CMSG_CAST_SPELL.
local Dbc = __AndroidDbc
local spells = {}       -- id -> {name, rank, icon, passive, ...}
local book = {}         -- flat spellbook: index -> spell id (tabs are contiguous ranges)
local tabs = {}         -- {name, icon, offset, count}
local known = {}        -- spell id -> true
local skillOf, supersededBy = nil, nil  -- SkillLineAbility: spell -> skill line / next rank

local function spellIcon(iconId)
  local path = iconId and iconId > 0 and Dbc("SpellIcon", iconId, "textureFilename")
  if path and path ~= "" then return path end
  return "Interface\\Icons\\INV_Misc_QuestionMark"
end
local function spellData(id)
  id = tonumber(id)
  if not id then return end
  local s = spells[id]
  if s then return s end
  local name, rank, iconId, attributes, powerType, manaCost, castIndex, rangeIndex, level, description =
    Dbc("Spell", id, "name", "nameSubtext", "spellIconID", "attributes", "powerType", "manaCost",
        "castingTimeIndex", "rangeIndex", "spellLevel", "description")
  if not name then return end
  s = { id = id, name = name, rank = rank or "", icon = spellIcon(iconId), passive = bit.band(attributes or 0, 0x40) ~= 0, attributes = attributes or 0,
        powerType = powerType or 0, cost = manaCost or 0, level = level or 0, description = description or "" }
  s.castTime = castIndex and castIndex > 0 and (Dbc("SpellCastTimes", castIndex, "base") or 0) or 0
  if rangeIndex and rangeIndex > 0 then s.minRange, s.maxRange = Dbc("SpellRange", rangeIndex, "rangeMin", "rangeMax") end
  spells[id] = s
  return s
end
__AndroidSpellData = spellData

local function loadSkillAbilities()
  if skillOf then return end
  skillOf, supersededBy = {}, {}
  local rows = __AndroidDbcCount("SkillLineAbility")
  for i = 1, rows do
    local line, spell, classMask, next = __AndroidDbcRow("SkillLineAbility", i, "skillLine", "spell", "classMask", "supercededBySpell")
    if spell and not skillOf[spell] then
      skillOf[spell] = { line = line, classMask = classMask or 0 }
      if next and next > 0 then supersededBy[spell] = next end
    end
  end
end

-- Abilities shown in General without a skill line.
local HIDDEN_CATEGORY = { [6] = true, [8] = true, [10] = true }
local ALWAYS = { [6603] = true, [75] = true, [5019] = true, [2764] = true, [3018] = true }
local function rebuild()
  loadSkillAbilities()
  known = {}
  local list = { __AndroidSpells() }
  for _, id in ipairs(list) do known[id] = true end
  local general, byLine, lines = {}, {}, {}
  local _, _, classId = UnitClass("player")
  local classBit = classId and bit.lshift(1, classId - 1) or 0
  for _, id in ipairs(list) do
    local s = spellData(id)
    local ability = skillOf[id]
    local next = supersededBy[id]
    if s and not (next and known[next]) and (ability or ALWAYS[id]) then
      local category = ability and Dbc("SkillLine", ability.line, "categoryID")
      if ability and category == 7 and bit.band(ability.classMask, classBit) ~= 0 then
        if not byLine[ability.line] then byLine[ability.line] = {}; lines[#lines + 1] = ability.line end
        tinsert(byLine[ability.line], id)
      elseif not ability or not HIDDEN_CATEGORY[category or 0] then
        -- Weapon/armor proficiencies and languages are not spellbook entries.
        if bit.band(s.attributes or 0, 0x80) == 0 then tinsert(general, id) end
      end
    end
  end
  local function order(a, b)
    local sa, sb = spells[a], spells[b]
    if sa.passive ~= sb.passive then return not sa.passive end
    if sa.level ~= sb.level then return sa.level < sb.level end
    return sa.name < sb.name
  end
  sort(general, order)
  sort(lines)
  book, tabs = {}, {}
  local function addTab(name, icon, ids)
    tabs[#tabs + 1] = { name = name, icon = icon, offset = #book, count = #ids }
    for _, id in ipairs(ids) do book[#book + 1] = id end
  end
  addTab(GENERAL_SPELLS or "General", "Interface\\Icons\\INV_Misc_Book_09", general)
  for _, line in ipairs(lines) do
    local ids = byLine[line]
    sort(ids, order)
    local name, iconId = Dbc("SkillLine", line, "displayName", "spellIconID")
    addTab(name or "?", spellIcon(iconId), ids)
  end
end
function __AndroidSpellsChanged() rebuild() end

-- Spellbook API (bookType "spell"; the pet book is empty).
local function bookSpell(index, bookType)
  if bookType == "pet" or bookType == BOOKTYPE_PET then return end
  local id = book[tonumber(index) or 0]
  return id and spellData(id)
end
function GetNumSpellTabs() if #tabs == 0 then rebuild() end; return #tabs end
function GetSpellTabInfo(tab)
  if #tabs == 0 then rebuild() end
  local t = tabs[tonumber(tab) or 0]
  if t then return t.name, t.icon, t.offset, t.count end
end
function GetSpellName(index, bookType)
  local s = bookSpell(index, bookType)
  if s then return s.name, s.rank end
end
function GetSpellBookItemName(index, bookType) return GetSpellName(index, bookType) end
local function resolve(spell, bookType)
  if type(spell) == "number" and bookType then return bookSpell(spell, bookType) end
  if type(spell) == "number" then return spellData(spell) end
  if type(spell) == "string" then
    local id = strmatch(spell, "spell:(%d+)")
    if id then return spellData(tonumber(id)) end
    local name = strmatch(spell, "^([^%(]+)") or spell
    name = gsub(name, "%s+$", "")
    local best
    for _, sid in ipairs(book) do
      local s = spellData(sid)
      if s and s.name == name then best = s end
    end
    return best
  end
end
function GetSpellTexture(spell, bookType) local s = resolve(spell, bookType); return s and s.icon end
function GetSpellInfo(spell, bookType)
  local s = resolve(spell, bookType)
  if not s then return end
  return s.name, s.rank, s.icon, s.cost, nil, s.powerType, s.castTime, s.minRange or 0, s.maxRange or 0
end
function GetSpellLink(spell, bookType)
  local s = resolve(spell, bookType)
  if s then return format("|cff71d5ff|Hspell:%d|h[%s]|h|r", s.id, s.name), "" end
end
function IsPassiveSpell(spell, bookType) local s = resolve(spell, bookType); if s and s.passive then return 1 end end
function IsSpellKnown(id) if known[tonumber(id) or 0] then return true end; return false end
function GetSpellCooldown() return 0, 0, 1 end
function IsUsableSpell(spell, bookType) if resolve(spell, bookType) then return 1, nil end end
function IsCurrentSpell() return nil end
function IsAutoRepeatSpell() return nil end
function IsAttackSpell(spell, bookType) local s = resolve(spell, bookType); if s and s.id == 6603 then return 1 end end
function SpellHasRange(spell, bookType) local s = resolve(spell, bookType); if s and (s.maxRange or 0) > 0 then return 1 end end
function IsSpellInRange() return nil end
function GetSpellCount() return 0 end
function GetSpellAutocast() return nil, nil end
function IsSelectedSpell() return nil end
function GetNumSpellBookSkillLines() return #tabs end
function UpdateSpells() end
function GetKnownSlotFromHighestRankSlot(slot) return slot end
function GetSpellDescription(id) local s = spellData(id); return s and s.description end

-- Casting: target the current target when there is one, else self (mask 0).
local castCount = 0
function __AndroidCast(id, unit)
  id = tonumber(id)
  if not id then return end
  if id == 6603 then -- Attack: auto attack toggle
    local guid = UnitGUID(unit or "target")
    if guid then __AndroidSend(OP.CMSG_ATTACK_SWING, "l", guid) end
    return
  end
  castCount = (castCount + 1) % 256
  local guid = UnitGUID(unit or "target")
  if guid and not UnitIsUnit(unit or "target", "player") then
    __AndroidSend(OP.CMSG_CAST_SPELL, "bibip", castCount, id, 0, 0x2, guid)
  else
    __AndroidSend(OP.CMSG_CAST_SPELL, "bibi", castCount, id, 0, 0)
  end
end
function CastSpell(index, bookType) local s = bookSpell(index, bookType); if s and not s.passive then __AndroidCast(s.id) end end
function CastSpellByName(name, unit) local s = resolve(name); if s and not s.passive then __AndroidCast(s.id, unit) end end
function CastSpellByID(id, unit) __AndroidCast(id, unit) end
function SpellStopCasting() __AndroidSend(OP.CMSG_CANCEL_CAST, "bi", 0, 0) end
function StopAttack() __AndroidSend(OP.CMSG_ATTACK_STOP, "") end
function StartAttack(unit) __AndroidCast(6603, unit) end
function PickupSpell(spell, bookType)
  local s = resolve(spell, bookType)
  if s and not s.passive then
    __AndroidSetCursor({ kind = "spell", id = s.id, index = spell, book = bookType or "spell" })
  end
end
