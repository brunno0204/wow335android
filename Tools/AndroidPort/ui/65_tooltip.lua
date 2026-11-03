-- Android port: GameTooltip behaviour (CGTooltip is not compiled in): lines on the template's
-- $parentTextLeftN/RightN font strings, sizing on Show, and the item/spell/action/unit setters.
local tooltips = setmetatable({}, { __mode = "k" })
local frameMethods = getmetatable(CreateFrame("Frame")).__index
local baseShow, baseHide = frameMethods.Show, frameMethods.Hide

local function line(tt, side, n)
  local name = tt:GetName()
  local fs = name and _G[name .. "Text" .. side .. n]
  if fs or not name then return fs end
  -- Lines past the template's: create them like CGTooltip does.
  fs = tt:CreateFontString(name .. "Text" .. side .. n, "ARTWORK", "GameTooltipText")
  if side == "Left" then
    fs:SetPoint("TOPLEFT", _G[name .. "TextLeft" .. (n - 1)], "BOTTOMLEFT", 0, -2)
  else
    fs:SetPoint("RIGHT", _G[name .. "TextLeft" .. n], "LEFT", 40, 0)
  end
  return fs
end
local function state(tt) local s = tooltips[tt]; if not s then s = { lines = 0 }; tooltips[tt] = s end; return s end
local QUALITY_NAME_COLOR = function(q) local r, g, b = GetItemQualityColor(q or 1); return r, g, b end

local T = {}
function T:ClearLines()
  local s = state(self)
  for n = 1, s.lines do
    local l, r = line(self, "Left", n), line(self, "Right", n)
    if l then l:SetText(""); l:Hide() end
    if r then r:SetText(""); r:Hide() end
  end
  s.lines = 0
end
function T:AddLine(text, r, g, b, wrap)
  local s = state(self)
  local n = s.lines + 1
  local left = line(self, "Left", n)
  if not left then return end
  s.lines = n
  left:SetText(text or "")
  left:SetTextColor(r or 1, g or 0.82, b or 0)
  left:Show()
  local right = line(self, "Right", n)
  if right then right:SetText(""); right:Hide() end
end
function T:AddDoubleLine(left, right, lr, lg, lb, rr, rg, rb)
  self:AddLine(left, lr, lg, lb)
  local fs = line(self, "Right", state(self).lines)
  if fs then
    fs:SetText(right or "")
    fs:SetTextColor(rr or 1, rg or 0.82, rb or 0)
    fs:Show()
  end
end
function T:SetText(text, r, g, b)
  self:ClearLines()
  self:AddLine(text, r or 1, g or 1, b or 1)
  self:Show()
end
function T:AppendText(text)
  local fs = line(self, "Left", 1)
  if fs then fs:SetText((fs:GetText() or "") .. (text or "")) end
end
function T:NumLines() return state(self).lines end
function T:SetOwner(owner, anchor, x, y)
  local s = state(self)
  s.owner = owner
  self:ClearLines()
  self:ClearAllPoints()
  anchor = anchor or "ANCHOR_LEFT"
  x, y = x or 0, y or 0
  if anchor == "ANCHOR_NONE" or not owner then return end
  if anchor == "ANCHOR_RIGHT" then self:SetPoint("BOTTOMLEFT", owner, "TOPRIGHT", x, y)
  elseif anchor == "ANCHOR_LEFT" then self:SetPoint("BOTTOMRIGHT", owner, "TOPLEFT", x, y)
  elseif anchor == "ANCHOR_TOPLEFT" then self:SetPoint("BOTTOMLEFT", owner, "TOPLEFT", x, y)
  elseif anchor == "ANCHOR_BOTTOMLEFT" then self:SetPoint("TOPRIGHT", owner, "BOTTOMLEFT", x, y)
  elseif anchor == "ANCHOR_BOTTOMRIGHT" then self:SetPoint("TOPLEFT", owner, "BOTTOMRIGHT", x, y)
  elseif anchor == "ANCHOR_BOTTOM" then self:SetPoint("TOP", owner, "BOTTOM", x, y)
  else self:SetPoint("BOTTOMLEFT", owner, "TOPRIGHT", x, y) end
end
function T:GetOwner() return state(self).owner end
function T:IsOwned(frame) if state(self).owner == frame and self:IsShown() then return 1 end end
function T:SetMinimumWidth(width) state(self).minWidth = width end
function T:SetPadding(padding) state(self).padding = padding end
function T:Show()
  local s = state(self)
  local width, height = s.minWidth or 0, 0
  for n = 1, s.lines do
    local l, r = line(self, "Left", n), line(self, "Right", n)
    local w = l and l:GetStringWidth() or 0
    if r and r:IsShown() and (r:GetText() or "") ~= "" then w = w + r:GetStringWidth() + 20 end
    width = max(width, w)
    height = height + (l and l:GetHeight() or 12) + 2
  end
  self:SetWidth(width + 20 + (s.padding or 0))
  self:SetHeight(height + 18)
  -- Right column flush with the tooltip's right edge (CGTooltip layout).
  for n = 1, s.lines do
    local l, r = line(self, "Left", n), line(self, "Right", n)
    if l and r and r:IsShown() then
      r:ClearAllPoints()
      r:SetPoint("RIGHT", l, "LEFT", width, 0)
    end
  end
  baseShow(self)
end
function T:FadeOut() baseHide(self) end
function T:SetUnit(unit)
  if not UnitExists(unit) then return end
  self:ClearLines()
  self:AddLine(UnitName(unit), 1, 1, 1)
  local level = UnitLevel(unit)
  local class = UnitClass(unit)
  if UnitIsPlayer(unit) then
    self:AddLine(format(TOOLTIP_UNIT_LEVEL_CLASS or "Level %s %s", level > 0 and level or "??", class or ""), 1, 1, 1)
  else
    self:AddLine(format(TOOLTIP_UNIT_LEVEL or "Level %s", level > 0 and level or "??"), 1, 1, 1)
  end
  self:Show()
end

-- Items.
local BIND = { [1] = "ITEM_BIND_ON_PICKUP", [2] = "ITEM_BIND_ON_EQUIP", [3] = "ITEM_BIND_ON_USE", [4] = "ITEM_BIND_QUEST" }
local STATS = { [3] = "ITEM_MOD_AGILITY", [4] = "ITEM_MOD_STRENGTH", [5] = "ITEM_MOD_INTELLECT", [6] = "ITEM_MOD_SPIRIT",
                [7] = "ITEM_MOD_STAMINA" }
local itemLines
itemLines = function(tt, entry, count, durability, maxDurability)
  local t = __AndroidItemTemplateTable(entry)
  if not t then tt:AddLine(RETRIEVING_ITEM_INFO or "Retrieving item information", 1, 0.1, 0.1); return end
  tt:AddLine(t.name, QUALITY_NAME_COLOR(t.quality))
  if BIND[t.bonding] then tt:AddLine(_G[BIND[t.bonding]], 1, 1, 1) end
  local _, _, _, _, _, _, subclass, _, equipLoc = GetItemInfo(entry)
  local slot = equipLoc and equipLoc ~= "" and _G[equipLoc]
  if slot then tt:AddDoubleLine(slot, subclass or "", 1, 1, 1, 1, 1, 1) end
  if t.dmgMax and t.dmgMax > 0 then
    local speed = (t.delay or 0) / 1000
    tt:AddDoubleLine(format(DAMAGE_TEMPLATE or "%d - %d Damage", t.dmgMin, t.dmgMax),
                     format("%s %.2f", SPEED or "Speed", speed), 1, 1, 1, 1, 1, 1)
    if speed > 0 then tt:AddLine(format(DPS_TEMPLATE or "(%.1f damage per second)", (t.dmgMin + t.dmgMax) / 2 / speed), 1, 1, 1) end
  end
  if t.armor and t.armor > 0 then tt:AddLine(format(ARMOR_TEMPLATE or "%d Armor", t.armor), 1, 1, 1) end
  local stats = { __AndroidItemStats(entry) }
  for i = 1, #stats, 2 do
    local key = STATS[stats[i]]
    if key and _G[key] then
      local v = stats[i + 1]
      tt:AddLine(format(_G[key], v >= 0 and 43 or 45, abs(v)), 1, 1, 1)
    end
  end
  if t.slots and t.slots > 0 then tt:AddLine(format(CONTAINER_SLOTS or "%d Slot %s", t.slots, BAGSLOT or "Bag"), 1, 1, 1) end
  if maxDurability and maxDurability > 0 then
    tt:AddLine(format(DURABILITY_TEMPLATE or "Durability %d / %d", durability or 0, maxDurability), 1, 1, 1)
  end
  if t.reqLevel and t.reqLevel > 1 then
    local r = (UnitLevel("player") or 0) < t.reqLevel and 1 or 1
    tt:AddLine(format(ITEM_MIN_LEVEL or "Requires Level %d", t.reqLevel), r, (UnitLevel("player") or 0) < t.reqLevel and 0.1 or 1, (UnitLevel("player") or 0) < t.reqLevel and 0.1 or 1)
  end
  if t.description and t.description ~= "" then tt:AddLine('"' .. t.description .. '"', 1, 0.82, 0, 1) end
end
function T:SetHyperlink(link)
  self:ClearLines()
  local entry = __AndroidItemEntry(link)
  if entry and strfind(tostring(link), "item:") or type(link) == "number" then itemLines(self, entry)
  else
    local spell = strmatch(tostring(link), "spell:(%d+)")
    if spell then self:SetSpellByID(tonumber(spell)); return end
  end
  self:Show()
end
function T:SetBagItem(bag, slot)
  local guid = __AndroidItemLocation(bag, slot)
  if not guid then return end
  local entry, count, durability, maxDurability = __AndroidItemObject(guid)
  if not entry then return end
  self:ClearLines()
  itemLines(self, entry, count, durability, maxDurability)
  self:Show()
  return nil, nil, nil
end
function T:SetInventoryItem(unit, slot)
  local entry = GetInventoryItemID(unit, slot)
  if not entry then return end
  local durability, maxDurability = GetInventoryItemDurability(slot)
  self:ClearLines()
  itemLines(self, entry, 1, durability, maxDurability)
  self:Show()
  return 1, nil, nil
end

-- Spells and actions.
local POWER_COST = { [0] = "MANA_COST", [1] = "RAGE_COST", [3] = "ENERGY_COST", [6] = "RUNIC_POWER_COST" }
function T:SetSpellByID(id)
  local s = __AndroidSpellData(id)
  if not s then return end
  self:ClearLines()
  self:AddDoubleLine(s.name, s.rank or "", 1, 1, 1, 0.5, 0.5, 0.5)
  if s.passive then
    self:AddLine(SPELL_PASSIVE or "Passive", 1, 1, 1)
  else
    local costKey = POWER_COST[s.powerType]
    local cost = (s.cost or 0) > 0 and costKey and _G[costKey]
    local amount = s.powerType == 1 and floor(s.cost / 10) or s.cost
    if cost then self:AddLine(format(cost, amount), 1, 1, 1) end
    if (s.castTime or 0) > 0 then self:AddLine(format(SPELL_CAST_TIME_SEC or "%.3g sec cast", s.castTime / 1000), 1, 1, 1)
    else self:AddLine(SPELL_CAST_TIME_INSTANT or "Instant cast", 1, 1, 1) end
  end
  if s.description and s.description ~= "" then self:AddLine(s.description, 1, 0.82, 0, 1) end
  self:Show()
end
function T:SetSpell(index, bookType)
  local name = GetSpellName(index, bookType)
  if not name then return end
  local link = GetSpellLink(index, bookType)
  local id = link and tonumber(strmatch(link, "spell:(%d+)"))
  if id then self:SetSpellByID(id) end
end
function T:SetAction(slot)
  local kind, id = GetActionInfo(slot)
  if kind == "spell" then self:SetSpellByID(id); return 1 end
  if kind == "item" then self:ClearLines(); itemLines(self, id); self:Show(); return 1 end
end
for _, name in ipairs({ "SetPetAction", "SetQuestLogItem",
    "SetTalent", "SetTradePlayerItem", "SetTradeTargetItem",
    "SetSendMailItem", "SetInboxItem", "SetAuctionItem",
    "SetQuestRewardSpell", "SetQuestLogRewardSpell", "SetBackpackToken", "SetCurrencyToken",
    "SetTotem", "SetGlyph", "SetEquipmentSet", "SetMerchantCostItem",
    "SetGuildBankItem", "SetSocketGem", "SetExistingSocketGem", "SetTracking", "SetFrameStack" }) do
  T[name] = function() end
end


-- Spell description tokens ($s1, $d, $o1, $t1, $h, $u, $x1, $a1, other spells' $12345s1).
local Dbc = __AndroidDbc
local function durationOf(spell)
  local index = Dbc("Spell", spell, "durationIndex")
  local ms = index and index > 0 and Dbc("SpellDuration", index, "duration") or 0
  return ms or 0
end
local function seconds(ms)
  local s = (ms or 0) / 1000
  if s >= 60 and s % 60 == 0 then return format("%d min", s / 60) end
  return format("%g sec", s)
end
local function points(spell, i)
  local base = Dbc("Spell", spell, "effectBasePoints" .. i) or 0
  local sides = Dbc("Spell", spell, "effectDieSides" .. i) or 0
  if sides > 1 then return format("%d to %d", base + 1, base + sides) end
  return tostring(abs(base + (sides > 0 and 1 or 0)))
end
local function token(spell, kind, index)
  index = tonumber(index) or 1
  if kind == "s" or kind == "m" then return points(spell, index) end
  if kind == "d" then return seconds(durationOf(spell)) end
  if kind == "t" then
    local period = Dbc("Spell", spell, "effectAuraPeriod" .. index) or 0
    return format("%g", period / 1000)
  end
  if kind == "o" then
    local period = Dbc("Spell", spell, "effectAuraPeriod" .. index) or 0
    local each = (Dbc("Spell", spell, "effectBasePoints" .. index) or 0) + 1
    if period > 0 then return tostring(floor(each * durationOf(spell) / period)) end
    return tostring(each)
  end
  if kind == "h" then return tostring(Dbc("Spell", spell, "procChance") or 0) end
  if kind == "u" then return tostring(Dbc("Spell", spell, "cumulativeAura") or 0) end
  if kind == "x" then return tostring(Dbc("Spell", spell, "effectChainTargets" .. index) or 0) end
  if kind == "a" then
    local r = Dbc("Spell", spell, "effectRadiusIndex" .. index)
    return tostring(r and r > 0 and floor(Dbc("SpellRadius", r, "radius") or 0) or 0)
  end
  if kind == "n" then return tostring(Dbc("Spell", spell, "procCharges") or 0) end
  return ""
end
function __AndroidSpellText(spell, text)
  if not text or text == "" then return "" end
  text = gsub(text, "%$(%d+)([sdtohuxamn])(%d?)", function(other, kind, index) return token(tonumber(other), kind, index) end)
  text = gsub(text, "%$([sdtohuxamn])(%d?)", function(kind, index) return token(spell, kind, index) end)
  text = gsub(text, "%$l([^:;]+):([^;]+);", "%2")
  text = gsub(text, "%${[^}]*}", "")
  text = gsub(text, "%$%*?[%d%.]*;", "")
  return text
end
local function rangeText(spell)
  local index = Dbc("Spell", spell, "rangeIndex")
  if not index or index <= 1 then return nil end
  local max = Dbc("SpellRange", index, "rangeMax") or 0
  if max <= 5.5 then return MELEE_RANGE or "Melee Range" end
  return format(SPELL_RANGE or "%s yd range", floor(max))
end
local function cooldownText(spell)
  local ms = max(Dbc("Spell", spell, "recoveryTime") or 0, Dbc("Spell", spell, "categoryRecoveryTime") or 0)
  if ms <= 1500 then return nil end
  local s = ms / 1000
  if s >= 60 then return format(SPELL_RECAST_TIME_MIN or "%.3g min cooldown", s / 60) end
  return format(SPELL_RECAST_TIME_SEC or "%.3g sec cooldown", s)
end
function T:SetSpellByID(id)
  local s = __AndroidSpellData(id)
  if not s then return end
  self:ClearLines()
  self:AddDoubleLine(s.name, s.rank or "", 1, 1, 1, 0.5, 0.5, 0.5)
  if s.passive then
    self:AddLine(SPELL_PASSIVE or "Passive", 1, 1, 1)
  else
    local costKey = POWER_COST[s.powerType]
    local cost = (s.cost or 0) > 0 and costKey and _G[costKey]
    local amount = s.powerType == 1 and floor(s.cost / 10) or s.cost
    local range = rangeText(id)
    if cost or range then self:AddDoubleLine(cost and format(cost, amount) or "", range or "", 1, 1, 1, 1, 1, 1) end
    local castText = (s.castTime or 0) > 0 and format(SPELL_CAST_TIME_SEC or "%.3g sec cast", s.castTime / 1000)
                     or (SPELL_CAST_TIME_INSTANT or "Instant cast")
    self:AddDoubleLine(castText, cooldownText(id) or "", 1, 1, 1, 1, 1, 1)
  end
  local text = __AndroidSpellText(id, s.description)
  if text ~= "" then self:AddLine(text, 1, 0.82, 0, 1) end
  self:Show()
end

-- Item extras: on-use/equip spells, sell price at a vendor.
local itemLinesBase = itemLines
itemLines = function(tt, entry, count, durability, maxDurability)
  itemLinesBase(tt, entry, count, durability, maxDurability)
  local t = __AndroidItemTemplateTable(entry)
  if not t then return end
  local spells = { __AndroidItemSpells(entry) }
  local TRIGGER = { [0] = ITEM_SPELL_TRIGGER_ONUSE or "Use:", [1] = ITEM_SPELL_TRIGGER_ONEQUIP or "Equip:",
                    [2] = ITEM_SPELL_TRIGGER_ONPROC or "Chance on hit:" }
  for i = 1, #spells, 2 do
    local spell, trigger = spells[i], spells[i + 1]
    if spell and spell > 0 and TRIGGER[trigger] then
      local desc = __AndroidSpellText(spell, Dbc("Spell", spell, "description"))
      if desc ~= "" then tt:AddLine(TRIGGER[trigger] .. " " .. desc, 0, 1, 0, 1) end
    end
  end
  if __AndroidMerchantOpen and __AndroidMerchantOpen() and (t.sellPrice or 0) > 0 then
    tt:AddLine((SELL_PRICE or "Sell Price") .. ": " .. GetCoinText(t.sellPrice * max(1, count or 1), " "), 1, 1, 1)
  end
end
local function linkEntry(link) return link and tonumber(strmatch(link, "item:(%d+)")) end
local function itemTooltip(tt, link, count)
  local entry = linkEntry(link)
  if not entry then return end
  tt:ClearLines()
  itemLines(tt, entry, count)
  tt:Show()
end
function T:SetLootItem(slot) itemTooltip(self, GetLootSlotLink(slot)) end
function T:SetQuestItem(kind, index) itemTooltip(self, GetQuestItemLink(kind, index)) end
function T:SetMerchantItem(index) itemTooltip(self, GetMerchantItemLink(index)) end
function T:SetBuybackItem() end
function T:SetTrainerService(index)
  local icon = GetTrainerServiceIcon(index)
  local link = GetTrainerServiceInfo(index) and __AndroidTrainerSpell and __AndroidTrainerSpell(index)
  if link then self:SetSpellByID(link) end
end
function T:SetShapeshift(index)
  local _, name = GetShapeshiftFormInfo(index)
  if name then self:SetSpellByID(select(1, __AndroidFormSpell and __AndroidFormSpell(index))) end
end
local function auraTooltip(tt, unit, index, filter)
  local name, rank, icon, count, debuffType, duration, expires, caster, _, _, spell = UnitAura(unit, index, filter)
  if not spell then return end
  tt:ClearLines()
  local r, g, b = 1, 0.82, 0
  tt:AddLine(name, 1, 1, 1)
  local aura = Dbc("Spell", spell, "auraDescription")
  local text = __AndroidSpellText(spell, aura and aura ~= "" and aura or Dbc("Spell", spell, "description"))
  if text ~= "" then tt:AddLine(text, r, g, b, 1) end
  if expires and expires > 0 then
    local left = expires - GetTime()
    if left > 0 then tt:AddLine(format(SecondsToTimeAbbrev and "%s" or "%d", SecondsToTimeAbbrev and format(SecondsToTimeAbbrev(left)) or floor(left)) .. " " .. (REMAINING or "remaining"), 1, 1, 1) end
  end
  tt:Show()
end
function T:SetUnitAura(unit, index, filter) auraTooltip(self, unit, index, filter) end
function T:SetUnitBuff(unit, index, filter) auraTooltip(self, unit, index, "HELPFUL" .. (filter and ("|" .. filter) or "")) end
function T:SetUnitDebuff(unit, index, filter) auraTooltip(self, unit, index, "HARMFUL" .. (filter and ("|" .. filter) or "")) end
function T:SetUnit(unit)
  if not UnitExists(unit) then return end
  self:ClearLines()
  local r, g, b = 1, 1, 1
  if not UnitIsPlayer(unit) then
    local reaction = UnitReaction(unit, "player") or 4
    if reaction <= 3 then r, g, b = 1, 0.1, 0.1 elseif reaction == 4 then r, g, b = 1, 1, 0 else r, g, b = 0.1, 1, 0.1 end
  end
  self:AddLine(UnitName(unit), r, g, b)
  local level = UnitLevel(unit)
  if UnitIsPlayer(unit) then
    local race = UnitRace(unit)
    self:AddLine(format(TOOLTIP_UNIT_LEVEL_RACE_CLASS or "Level %s %s %s", level > 0 and level or "??", race or "",
                        (UnitClass(unit)) or ""), 1, 1, 1)
  else
    local kind = UnitCreatureType(unit)
    self:AddLine(format(TOOLTIP_UNIT_LEVEL_TYPE or "Level %s %s", level > 0 and level or "??", kind or ""), 1, 1, 1)
  end
  if UnitIsDead(unit) then self:AddLine(CORPSE or "Corpse", 0.5, 0.5, 0.5) end
  self:Show()
end

-- Install on every tooltip (FrameXML GameTooltip frames are plain frames here).
local function install(frame)
  if not frame or tooltips[frame] then return end
  state(frame)
  for name, fn in pairs(T) do frame[name] = fn end
end
__AndroidInstallTooltip = install
local createFrame = CreateFrame
function CreateFrame(kind, name, parent, template, ...)
  local frame = createFrame(kind, name, parent, template, ...)
  if kind == "GameTooltip" then install(frame) end
  return frame
end
function __AndroidInstallTooltips()
  local f = EnumerateFrames()
  while f do
    local name = f:GetName()
    if name and _G[name .. "TextLeft1"] and _G[name .. "TextRight1"] then install(f) end
    f = EnumerateFrames(f)
  end
end
