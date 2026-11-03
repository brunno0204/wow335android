-- Android port: original API functions whose "no data" answer is not what a generic stub
-- (90_stubs.lua) gives.
function GetMirrorTimerInfo() return "UNKNOWN", 0, 0, 0, 0, "" end
function GetMirrorTimerProgress() return 0 end
function GetLootMethod() return "freeforall", nil, nil end
function GetLootThreshold() return 2 end
function GetBattlefieldStatus() return "none", nil, 0, 0, 0, 0, nil end
-- Character sheet values from the player's unit fields (CGUnitData layout).
local function mine(unit) return not unit or UnitIsUnit(unit, "player") end
local function F(index, kind) return __AndroidPlayerField(index, kind) or 0 end
local function i16pair(index)
  local v = F(index)
  local low, high = v % 65536, floor(v / 65536)
  if low >= 32768 then low = low - 65536 end
  if high >= 32768 then high = high - 65536 end
  return low, high
end
function UnitStat(unit, index)
  index = tonumber(index) or 1
  if not mine(unit) or index < 1 or index > 5 then return 0, 0, 0, 0 end
  local stat = F(0x54 + index - 1)
  return stat, stat, F(0x59 + index - 1), F(0x5e + index - 1)
end
function UnitArmor(unit)
  if not mine(unit) then return 0, 0, 0, 0, 0 end
  local armor = F(0x63)
  return armor, armor, armor, F(0x6a), F(0x71)
end
function UnitResistance(unit, school)
  school = tonumber(school) or 0
  if not mine(unit) then return 0, 0, 0, 0 end
  local value = F(0x63 + school)
  return value, value, F(0x6a + school), F(0x71 + school)
end
function UnitAttackSpeed(unit)
  if not mine(unit) then return 2, nil end
  local main, off = F(0x3e), F(0x3f)
  return main > 0 and main / 1000 or 2, off > 0 and off / 1000 or nil
end
function UnitRangedDamage(unit)
  if not mine(unit) then return 0, 0, 0, 0, 0, 1 end
  local speed = F(0x40)
  return speed > 0 and speed / 1000 or 0, F(0x81, "float"), F(0x82, "float"), 0, 0, 1
end
function UnitDamage(unit)
  if not mine(unit) then return 1, 2, 0, 0, 0, 0, 1 end
  return F(0x46, "float"), F(0x47, "float"), F(0x48, "float"), F(0x49, "float"), 0, 0, 1
end
function UnitAttackPower(unit)
  if not mine(unit) then return 0, 0, 0 end
  local pos, neg = i16pair(0x7c)
  return F(0x7b), pos, neg
end
function UnitRangedAttackPower(unit)
  if not mine(unit) then return 0, 0, 0 end
  local pos, neg = i16pair(0x7f)
  return F(0x7e), pos, neg
end
function UnitAttackBothHands() return 0, 0, 0, 0 end
function UnitRangedAttack() return 0, 0 end
function GetCritChance() return 0 end
function GetRangedCritChance() return 0 end
function GetDodgeChance() return 0 end
function GetParryChance() return 0 end
function GetBlockChance() return 0 end
function GetSpellCritChance() return 0 end
function GetSpellBonusDamage() return 0 end
function GetSpellBonusHealing() return 0 end
function GetCombatRating() return 0 end
function GetCombatRatingBonus() return 0 end
function GetManaRegen() return 0, 0 end
function GetUnitHealthModifier() return 1 end
function GetUnitPowerModifier() return 1 end
function GetPVPRankInfo() return nil, 0 end
function UnitCharacterPoints(unit) if mine(unit) then return __AndroidPlayerField(0x3FC) or 0, __AndroidPlayerField(0x3FD) or 0 end; return 0, 0 end

-- ChrClasses.dbc: class token -> localized name (Constants.lua fills LOCALIZED_CLASS_NAMES_*).
local CLASS_FALLBACK = { WARRIOR = "Warrior", PALADIN = "Paladin", HUNTER = "Hunter", ROGUE = "Rogue",
  PRIEST = "Priest", DEATHKNIGHT = "Death Knight", SHAMAN = "Shaman", MAGE = "Mage", WARLOCK = "Warlock",
  DRUID = "Druid" }
function FillLocalizedClassList(list, female)
  if type(list) ~= "table" then return end
  local rows = __AndroidDbcCount("ChrClasses")
  for i = 1, rows do
    local token, name, male, femaleName = __AndroidDbcRow("ChrClasses", i, "filename", "name", "nameMale", "nameFemale")
    if token and token ~= "" then
      local text = female and femaleName or male
      if not text or text == "" then text = name end
      list[token] = text ~= "" and text or CLASS_FALLBACK[token]
    end
  end
  for token, text in pairs(CLASS_FALLBACK) do if not list[token] then list[token] = text end end
  return list
end

-- Frames the client creates in code (CWorldMap::CreatePlayerArrowFrame).
CreateFrame("Frame", "PlayerArrowEffectFrame", UIParent or nil)
CreateFrame("Frame", "PlayerMiniArrowEffectFrame", UIParent or nil)
