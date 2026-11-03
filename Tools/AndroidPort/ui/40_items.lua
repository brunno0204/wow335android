-- Android port: items, bags, equipment and money from the player's update fields and the
-- item templates the server sent (SMSG_ITEM_QUERY_SINGLE_RESPONSE).
local Field = __AndroidPlayerField
local INV_SLOT = 0x144          -- PLAYER_FIELD_INV_SLOT_HEAD (2 fields per slot)
local PACK_FIRST, PACK_COUNT = 23, 16
local BANK_FIRST, BANK_COUNT = 39, 28
local BANKBAG_FIRST = 67
local KEYRING_FIRST, KEYRING_COUNT = 86, 32
local BAG_FIRST = 19             -- equipment slots 19..22 hold the four bags

local QUALITY = {
  [-1] = {1, 1, 1, "ffffffff"},
  [0] = {0.62, 0.62, 0.62, "ff9d9d9d"}, [1] = {1, 1, 1, "ffffffff"}, [2] = {0.12, 1, 0, "ff1eff00"},
  [3] = {0, 0.44, 0.87, "ff0070dd"}, [4] = {0.64, 0.21, 0.93, "ffa335ee"}, [5] = {1, 0.5, 0, "ffff8000"},
  [6] = {0.9, 0.8, 0.5, "ffe6cc80"}, [7] = {0.9, 0.8, 0.5, "ffe6cc80"},
}
function GetItemQualityColor(quality)
  local q = QUALITY[tonumber(quality) or 1] or QUALITY[1]
  return q[1], q[2], q[3], "|c" .. q[4]
end

local EQUIP_LOC = {
  [1] = "INVTYPE_HEAD", [2] = "INVTYPE_NECK", [3] = "INVTYPE_SHOULDER", [4] = "INVTYPE_BODY",
  [5] = "INVTYPE_CHEST", [6] = "INVTYPE_WAIST", [7] = "INVTYPE_LEGS", [8] = "INVTYPE_FEET",
  [9] = "INVTYPE_WRIST", [10] = "INVTYPE_HAND", [11] = "INVTYPE_FINGER", [12] = "INVTYPE_TRINKET",
  [13] = "INVTYPE_WEAPON", [14] = "INVTYPE_SHIELD", [15] = "INVTYPE_RANGED", [16] = "INVTYPE_CLOAK",
  [17] = "INVTYPE_2HWEAPON", [18] = "INVTYPE_BAG", [19] = "INVTYPE_TABARD", [20] = "INVTYPE_ROBE",
  [21] = "INVTYPE_WEAPONMAINHAND", [22] = "INVTYPE_WEAPONOFFHAND", [23] = "INVTYPE_HOLDABLE",
  [24] = "INVTYPE_AMMO", [25] = "INVTYPE_THROWN", [26] = "INVTYPE_RANGEDRIGHT", [27] = "INVTYPE_QUIVER",
  [28] = "INVTYPE_RELIC",
}
-- Inventory types -> equipment slot (0-based) for auto-equip.
local EQUIP_SLOT = { [1] = 0, [2] = 1, [3] = 2, [4] = 3, [5] = 4, [20] = 4, [6] = 5, [7] = 6, [8] = 7,
  [9] = 8, [10] = 9, [11] = 10, [12] = 12, [13] = 15, [21] = 15, [17] = 15, [14] = 16, [22] = 16, [23] = 16,
  [15] = 17, [25] = 17, [26] = 17, [28] = 17, [16] = 14, [19] = 18, [18] = 19 }

-- Item class / subclass names (ItemClass.dbc, ItemSubClass.dbc).
local classNames, subclassNames = {}, {}
local function className(class)
  if classNames[class] == nil then
    local rows = __AndroidDbcCount("ItemClass")
    for i = 1, rows do
      local id, name = __AndroidDbcRow("ItemClass", i, "classID", "className")
      if id then classNames[id] = name or "" end
    end
    if classNames[class] == nil then classNames[class] = "" end
  end
  return classNames[class]
end
local function subclassName(class, subclass)
  local key = class * 256 + subclass
  if subclassNames[key] == nil then
    local rows = __AndroidDbcCount("ItemSubClass")
    for i = 1, rows do
      local c, s, name, verbose = __AndroidDbcRow("ItemSubClass", i, "classID", "subClassID", "displayName", "verboseName")
      if c then subclassNames[c * 256 + s] = (verbose and verbose ~= "") and verbose or name or "" end
    end
    if subclassNames[key] == nil then subclassNames[key] = "" end
  end
  return subclassNames[key]
end
local icons = {}
local function displayIcon(display)
  if not display or display == 0 then return nil end
  local icon = icons[display]
  if icon == nil then
    local name = __AndroidDbc("ItemDisplayInfo", display, "inventoryIcon1")
    icon = (name and name ~= "") and ("Interface\\Icons\\" .. name) or false
    icons[display] = icon
  end
  return icon or nil
end
__AndroidItemIcon = displayIcon

-- Template by entry: table (cached per item-info change) or nil (query sent).
local templates = {}
local function template(entry)
  entry = tonumber(entry)
  if not entry or entry <= 0 then return nil end
  local t = templates[entry]
  if t then return t end
  local name, quality, ilevel, reqLevel, class, subclass, maxStack, invType, display, sellPrice, armor, delay,
        dmgMin, dmgMax, bonding, maxDurability, description, slots, buyPrice, dmgType = __AndroidItemTemplate(entry)
  if not name then return nil end
  t = { entry = entry, name = name, quality = quality, ilevel = ilevel, reqLevel = reqLevel, class = class,
        subclass = subclass, maxStack = maxStack, invType = invType, display = display, sellPrice = sellPrice,
        armor = armor, delay = delay, dmgMin = dmgMin, dmgMax = dmgMax, dmgType = dmgType, bonding = bonding,
        maxDurability = maxDurability, description = description, slots = slots, buyPrice = buyPrice }
  t.icon = displayIcon(display) or "Interface\\Icons\\INV_Misc_QuestionMark"
  local _, _, _, hex = GetItemQualityColor(quality)
  t.link = format("%s|Hitem:%d:0:0:0:0:0:0:0:%d|h[%s]|h|r", hex, entry, UnitLevel("player") or 0, name)
  templates[entry] = t
  return t
end
__AndroidItemTemplateTable = template
function __AndroidItemsInfoChanged() templates = {} end

-- Item references: number, "item:123", links, or names of known templates.
local function entryOf(item)
  if type(item) == "number" then return item end
  if type(item) ~= "string" then return nil end
  local id = strmatch(item, "item:(%d+)")
  if id then return tonumber(id) end
  if tonumber(item) then return tonumber(item) end
  for entry, t in pairs(templates) do if t.name == item then return entry end end
end
__AndroidItemEntry = entryOf

function GetItemInfo(item)
  local t = template(entryOf(item))
  if not t then return end
  return t.name, t.link, t.quality, t.ilevel, t.reqLevel, className(t.class), subclassName(t.class, t.subclass),
         t.maxStack, EQUIP_LOC[t.invType] or "", t.icon, t.sellPrice
end
function GetItemIcon(item) local t = template(entryOf(item)); return t and t.icon end
function GetItemQualityByID(item) local t = template(entryOf(item)); return t and t.quality end
function IsEquippableItem(item) local t = template(entryOf(item)); if t and EQUIP_SLOT[t.invType] then return 1 end end
function GetItemFamily() return 0 end
function GetItemSpell() return nil end
function IsConsumableItem() return nil end
function IsHelpfulItem() return nil end
function IsHarmfulItem() return nil end
function ItemHasRange() return nil end
function IsItemInRange() return nil end
function GetItemCooldown() return 0, 0, 1 end
function GetItemStats(link, list)
  local entry = entryOf(link)
  list = list or {}
  if not entry then return list end
  local ITEM_MOD = { [3] = "ITEM_MOD_AGILITY_SHORT", [4] = "ITEM_MOD_STRENGTH_SHORT", [5] = "ITEM_MOD_INTELLECT_SHORT",
    [6] = "ITEM_MOD_SPIRIT_SHORT", [7] = "ITEM_MOD_STAMINA_SHORT", [31] = "ITEM_MOD_HIT_RATING_SHORT",
    [32] = "ITEM_MOD_CRIT_RATING_SHORT", [35] = "ITEM_MOD_RESILIENCE_RATING_SHORT", [36] = "ITEM_MOD_HASTE_RATING_SHORT",
    [37] = "ITEM_MOD_EXPERTISE_RATING_SHORT", [38] = "ITEM_MOD_ATTACK_POWER_SHORT", [45] = "ITEM_MOD_SPELL_POWER_SHORT" }
  local stats = { __AndroidItemStats(entry) }
  for i = 1, #stats, 2 do
    local key = ITEM_MOD[stats[i]]
    if key then list[key] = (list[key] or 0) + stats[i + 1] end
  end
  return list
end

-- ---- Inventory slots (0-based indices into PLAYER_FIELD_INV_SLOT_HEAD).
local function slotGuid(index) return Field(INV_SLOT + index * 2, "guid") end
local function itemAt(guid)
  if not guid then return end
  local entry, count, durability, maxDurability, numSlots, flags = __AndroidItemObject(guid)
  if not entry or entry == 0 then return end
  return entry, count, durability, maxDurability, numSlots, flags
end
-- (bag, slot) -> item GUID, server bag byte, server slot byte.
local function location(bag, slot)
  bag, slot = tonumber(bag), tonumber(slot)
  if not bag or not slot or slot < 1 then return end
  if bag == 0 then
    if slot > PACK_COUNT then return end
    return slotGuid(PACK_FIRST + slot - 1), 255, PACK_FIRST + slot - 1
  elseif bag >= 1 and bag <= 4 then
    local container = slotGuid(BAG_FIRST + bag - 1)
    if not container then return end
    return __AndroidContainerSlot(container, slot), BAG_FIRST + bag - 1, slot - 1
  elseif bag == -1 then
    if slot > BANK_COUNT then return end
    return slotGuid(BANK_FIRST + slot - 1), 255, BANK_FIRST + slot - 1
  elseif bag >= 5 and bag <= 11 then
    local container = slotGuid(BANKBAG_FIRST + bag - 5)
    if not container then return end
    return __AndroidContainerSlot(container, slot), BANKBAG_FIRST + bag - 5, slot - 1
  elseif bag == -2 then
    if slot > KEYRING_COUNT then return end
    return slotGuid(KEYRING_FIRST + slot - 1), 255, KEYRING_FIRST + slot - 1
  end
end
__AndroidItemLocation = location

function GetContainerNumSlots(bag)
  bag = tonumber(bag)
  if bag == 0 then return PACK_COUNT end
  if bag == -1 then return BANK_COUNT end
  if bag == -2 then return KEYRING_COUNT end
  local index = (bag and bag >= 1 and bag <= 4) and (BAG_FIRST + bag - 1)
             or (bag and bag >= 5 and bag <= 11) and (BANKBAG_FIRST + bag - 5)
  if not index then return 0 end
  local _, _, _, _, numSlots = itemAt(slotGuid(index))
  return numSlots or 0
end
function GetContainerNumFreeSlots(bag)
  local free = 0
  for slot = 1, GetContainerNumSlots(bag) do
    if not location(bag, slot) then free = free + 1 end
  end
  return free, 0
end
function GetContainerItemInfo(bag, slot)
  local guid = location(bag, slot)
  local entry, count = itemAt(guid)
  if not entry then return end
  local t = template(entry)
  return t and t.icon or "Interface\\Icons\\INV_Misc_QuestionMark", count, nil, t and t.quality or 1,
         nil, nil, t and t.link
end
function GetContainerItemID(bag, slot) return (itemAt((location(bag, slot)))) end
function GetContainerItemLink(bag, slot)
  local entry = itemAt((location(bag, slot)))
  local t = entry and template(entry)
  return t and t.link
end
function GetContainerItemDurability(bag, slot)
  local _, _, durability, maxDurability = itemAt((location(bag, slot)))
  if maxDurability and maxDurability > 0 then return durability, maxDurability end
end
function GetContainerItemCooldown() return 0, 0, 1 end
function GetContainerItemQuestInfo() return nil, nil, nil end
function ContainerIDToInventoryID(bag)
  bag = tonumber(bag)
  if bag and bag >= 1 and bag <= 4 then return BAG_FIRST + bag end
  if bag and bag >= 5 and bag <= 11 then return BANKBAG_FIRST + bag - 4 end
end
function GetBagName(bag)
  if bag == 0 then return BACKPACK_TOOLTIP or "Backpack" end
  local id = ContainerIDToInventoryID(bag)
  local entry = id and itemAt(slotGuid(id - 1))
  local t = entry and template(entry)
  return t and t.name
end

-- Equipment (inventory slot IDs are 1-based: INVSLOT_HEAD = 1 ... bags 20-23).
local function inventoryEntry(unit, slot)
  if unit and not UnitIsUnit(unit, "player") then return end
  slot = tonumber(slot)
  if not slot or slot < 1 then return end
  return itemAt(slotGuid(slot - 1))
end
function GetInventoryItemID(unit, slot) return (inventoryEntry(unit, slot)) end
function GetInventoryItemTexture(unit, slot)
  local entry = inventoryEntry(unit, slot)
  local t = entry and template(entry)
  return t and t.icon or (entry and "Interface\\Icons\\INV_Misc_QuestionMark") or nil
end
function GetInventoryItemLink(unit, slot)
  local entry = inventoryEntry(unit, slot)
  local t = entry and template(entry)
  return t and t.link
end
function GetInventoryItemQuality(unit, slot)
  local entry = inventoryEntry(unit, slot)
  local t = entry and template(entry)
  return t and t.quality
end
function GetInventoryItemCount(unit, slot) local _, count = inventoryEntry(unit, slot); return count or 0 end
function GetInventoryItemDurability(slot)
  local _, _, durability, maxDurability = inventoryEntry("player", slot)
  if maxDurability and maxDurability > 0 then return durability, maxDurability end
end
function GetInventoryItemBroken(unit, slot)
  local _, _, durability, maxDurability = inventoryEntry(unit, slot)
  if maxDurability and maxDurability > 0 and durability == 0 then return 1 end
end
function GetInventoryItemCooldown() return 0, 0, 1 end
function GetInventoryItemsForSlot() return {} end
function IsInventoryItemLocked() return nil end
function GetInventoryAlertStatus() return 0 end
local SLOT_NAMES = { HEADSLOT = 1, NECKSLOT = 2, SHOULDERSLOT = 3, SHIRTSLOT = 4, CHESTSLOT = 5, WAISTSLOT = 6,
  LEGSSLOT = 7, FEETSLOT = 8, WRISTSLOT = 9, HANDSSLOT = 10, FINGER0SLOT = 11, FINGER1SLOT = 12,
  TRINKET0SLOT = 13, TRINKET1SLOT = 14, BACKSLOT = 15, MAINHANDSLOT = 16, SECONDARYHANDSLOT = 17,
  RANGEDSLOT = 18, TABARDSLOT = 19, BAG0SLOT = 20, BAG1SLOT = 21, BAG2SLOT = 22, BAG3SLOT = 23, AMMOSLOT = 0 }
local SLOT_TEXTURES = { HEADSLOT = "Head", NECKSLOT = "Neck", SHOULDERSLOT = "Shoulder", SHIRTSLOT = "Shirt",
  CHESTSLOT = "Chest", WAISTSLOT = "Waist", LEGSSLOT = "Legs", FEETSLOT = "Feet", WRISTSLOT = "Wrists",
  HANDSSLOT = "Hands", FINGER0SLOT = "Finger", FINGER1SLOT = "Finger", TRINKET0SLOT = "Trinket",
  TRINKET1SLOT = "Trinket", BACKSLOT = "Chest", MAINHANDSLOT = "MainHand", SECONDARYHANDSLOT = "SecondaryHand",
  RANGEDSLOT = "Ranged", TABARDSLOT = "Tabard", AMMOSLOT = "Ammo" }
function GetInventorySlotInfo(name)
  local key = strupper(tostring(name or ""))
  local id = SLOT_NAMES[key]
  if not id then return end
  local texture = SLOT_TEXTURES[key] and ("Interface\\PaperDoll\\UI-PaperDoll-Slot-" .. SLOT_TEXTURES[key])
  return id, texture, nil
end

function GetItemCount(item, includeBank)
  local entry = entryOf(item)
  if not entry then return 0 end
  local total = 0
  local bags = includeBank and {0, 1, 2, 3, 4, -1, 5, 6, 7, 8, 9, 10, 11} or {0, 1, 2, 3, 4}
  for _, bag in ipairs(bags) do
    for slot = 1, GetContainerNumSlots(bag) do
      local e, count = itemAt((location(bag, slot)))
      if e == entry then total = total + (count or 1) end
    end
  end
  for slot = 1, 19 do
    local e, count = inventoryEntry("player", slot)
    if e == entry then total = total + (count or 1) end
  end
  return total
end

-- Money and experience.
function GetMoney() return Field(0x492) or 0 end
function UnitXP(unit) if not unit or UnitIsUnit(unit, "player") then return Field(0x27A) or 0 end; return 0 end
function UnitXPMax(unit)
  if not unit or UnitIsUnit(unit, "player") then local v = Field(0x27B) or 0; return v > 0 and v or 1 end
  return 1
end
function GetXPExhaustion() local v = Field(0x491) or 0; if v > 0 then return v end end
function GetRestState() if GetXPExhaustion() then return 1, "Rested", 2 end; return 2, "Normal", 1 end

-- ---- Moving and using items (cursor holds one item location).
local cursor
function __AndroidCursor() return cursor end
function __AndroidSetCursor(value) cursor = value; __AndroidSignal("CURSOR_UPDATE") end
function CursorHasItem() if cursor and cursor.kind == "item" then return 1 end end
function CursorHasSpell() if cursor and cursor.kind == "spell" then return 1 end end
function CursorHasMoney() return nil end
function GetCursorInfo()
  if not cursor then return end
  if cursor.kind == "item" then return "item", cursor.entry, cursor.link end
  if cursor.kind == "spell" then return "spell", cursor.index, cursor.book, cursor.id end
  if cursor.kind == "action" then return cursor.actionType, cursor.id end
end
function ClearCursor() cursor = nil; __AndroidSignal("CURSOR_UPDATE") end
function ResetCursor() end
function GetCursorMoney() return 0 end
local function swap(bag, slot, toBag, toSlot)
  -- CMSG_SWAP_ITEM: destination bag/slot then source bag/slot; CMSG_SWAP_INV_ITEM for 255/255.
  if bag == 255 and toBag == 255 then
    __AndroidSend(OP.CMSG_SWAP_INV_ITEM, "bb", slot, toSlot)
  else
    __AndroidSend(OP.CMSG_SWAP_ITEM, "bbbb", toBag, toSlot, bag, slot)
  end
end
local function pickup(guid, bag, slot)
  if cursor and cursor.kind == "item" then
    if cursor.bag ~= bag or cursor.slot ~= slot then swap(cursor.bag, cursor.slot, bag, slot) end
    ClearCursor()
  elseif guid then
    local entry = itemAt(guid)
    local t = entry and template(entry)
    __AndroidSetCursor({ kind = "item", bag = bag, slot = slot, guid = guid, entry = entry, link = t and t.link })
  end
end
function PickupContainerItem(bag, slot)
  local guid, b, s = location(bag, slot)
  if b then pickup(guid, b, s) end
end
function PickupInventoryItem(slot)
  slot = tonumber(slot)
  if slot and slot >= 1 then pickup(slotGuid(slot - 1), 255, slot - 1) end
end
function PickupBagFromSlot(slot) PickupInventoryItem(slot) end
function PutItemInBackpack()
  if not (cursor and cursor.kind == "item") then return end
  for slot = 1, PACK_COUNT do
    if not location(0, slot) then swap(cursor.bag, cursor.slot, 255, PACK_FIRST + slot - 1); break end
  end
  ClearCursor()
end
function PutItemInBag(inventoryId)
  if not (cursor and cursor.kind == "item") then return end
  local bag = (tonumber(inventoryId) or 0) - BAG_FIRST
  if bag >= 1 and bag <= 4 then
    for slot = 1, GetContainerNumSlots(bag) do
      if not location(bag, slot) then swap(cursor.bag, cursor.slot, BAG_FIRST + bag - 1, slot - 1); break end
    end
  end
  ClearCursor()
end
function EquipCursorItem(slot)
  if cursor and cursor.kind == "item" then swap(cursor.bag, cursor.slot, 255, (tonumber(slot) or 1) - 1) end
  ClearCursor()
end
function AutoEquipCursorItem()
  if cursor and cursor.kind == "item" then __AndroidSend(OP.CMSG_AUTO_EQUIP_ITEM, "bb", cursor.bag, cursor.slot) end
  ClearCursor()
end
function DeleteCursorItem()
  if cursor and cursor.kind == "item" then
    local _, count = itemAt(cursor.guid)
    __AndroidSend(OP.CMSG_DESTROY_ITEM, "bbb", cursor.bag, cursor.slot, count or 1)
  end
  ClearCursor()
end
function SplitContainerItem() end
function UseContainerItem(bag, slot)
  local guid, b, s = location(bag, slot)
  local entry = itemAt(guid)
  if not entry then return end
  local t = template(entry)
  if t and EQUIP_SLOT[t.invType] then
    __AndroidSend(OP.CMSG_AUTO_EQUIP_ITEM, "bb", b, s)
    return
  end
  local spell = 0
  if t then
    local spells = { __AndroidItemSpells(entry) }
    for i = 1, #spells, 2 do if spells[i] > 0 and spells[i + 1] == 0 then spell = spells[i]; break end end
  end
  -- CMSG_USE_ITEM: bag, slot, cast count, spell, item GUID, glyph index, cast flags, targets (self).
  __AndroidSend(OP.CMSG_USE_ITEM, "bbbilibi", b, s, 0, spell, guid, 0, 0, 0)
end
function UseInventoryItem(slot)
  local guid = slotGuid((tonumber(slot) or 1) - 1)
  local entry = itemAt(guid)
  if not entry then return end
  local spells = { __AndroidItemSpells(entry) }
  local spell = 0
  for i = 1, #spells, 2 do if spells[i] > 0 and spells[i + 1] == 0 then spell = spells[i]; break end end
  __AndroidSend(OP.CMSG_USE_ITEM, "bbbilibi", 255, (tonumber(slot) or 1) - 1, 0, spell, guid, 0, 0, 0)
end
function UseItemByName(name)
  local entry = entryOf(name)
  for _, bag in ipairs({0, 1, 2, 3, 4}) do
    for slot = 1, GetContainerNumSlots(bag) do
      if GetContainerItemID(bag, slot) == entry then UseContainerItem(bag, slot); return end
    end
  end
end
function EquipItemByName(name)
  local entry = entryOf(name)
  for _, bag in ipairs({0, 1, 2, 3, 4}) do
    for slot = 1, GetContainerNumSlots(bag) do
      if GetContainerItemID(bag, slot) == entry then
        local _, b, s = location(bag, slot)
        __AndroidSend(OP.CMSG_AUTO_EQUIP_ITEM, "bb", b, s)
        return
      end
    end
  end
end
