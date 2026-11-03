-- Android port: action bars from SMSG_UPDATE_ACTION_BUTTONS (144 slots, id | type << 24);
-- PlaceAction/PickupAction send CMSG_SET_ACTION_BUTTON like the original client.
local ACTION_SPELL, ACTION_MACRO, ACTION_ITEM = 0, 64, 128
local function action(slot)
  local id, kind = __AndroidAction(tonumber(slot) or 0)
  return id, kind
end
local function itemIcon(entry)
  local _, _, _, _, _, _, _, _, _, icon = GetItemInfo(entry)
  return icon or "Interface\\Icons\\INV_Misc_QuestionMark"
end
function HasAction(slot) if action(slot) then return 1 end end
function GetActionTexture(slot)
  local id, kind = action(slot)
  if not id then return end
  if kind == ACTION_ITEM then return itemIcon(id) end
  if kind == ACTION_MACRO then return "Interface\\Icons\\INV_Misc_QuestionMark" end
  local s = __AndroidSpellData(id)
  return s and s.icon
end
function GetActionInfo(slot)
  local id, kind = action(slot)
  if not id then return end
  if kind == ACTION_ITEM then return "item", id end
  if kind == ACTION_MACRO then return "macro", id end
  return "spell", id, "spell"
end
function GetActionText(slot) return nil end
function GetActionCount(slot)
  local id, kind = action(slot)
  if id and kind == ACTION_ITEM then return GetItemCount(id) end
  return 0
end
function GetActionCooldown() return 0, 0, 0 end
function IsUsableAction(slot) if action(slot) then return 1, nil end end
function IsActionInRange() return nil end
function ActionHasRange() return nil end
function IsCurrentAction() return nil end
function IsAutoRepeatAction() return nil end
function IsAttackAction(slot) local id, kind = action(slot); if id == 6603 and kind == ACTION_SPELL then return 1 end end
function IsEquippedAction() return nil end
function IsConsumableAction(slot) local _, kind = action(slot); if kind == ACTION_ITEM then return 1 end end
function IsStackableAction(slot) return IsConsumableAction(slot) end
function GetActionBarPage() return CURRENT_ACTIONBAR_PAGE or 1 end
function GetBonusBarOffset() return 0 end
function GetActionBarToggles() return nil, nil, nil, nil end
function SetActionBarToggles() end
function ChangeActionBarPage(page) CURRENT_ACTIONBAR_PAGE = page; __AndroidSignal("ACTIONBAR_PAGE_CHANGED") end
function GetPossessInfo() return nil end

function UseAction(slot, unit)
  local id, kind = action(slot)
  if not id then return end
  if kind == ACTION_ITEM then UseItemByName(id)
  elseif kind == ACTION_SPELL then __AndroidCast(id, unit) end
end

local function setAction(slot, value)
  slot = tonumber(slot)
  if not slot or slot < 1 or slot > 144 then return end
  __AndroidSend(OP.CMSG_SET_ACTION_BUTTON, "bi", slot - 1, value)
  __AndroidSetAction(slot, value)
  __AndroidSignal("ACTIONBAR_SLOT_CHANGED", slot)
end
function PickupAction(slot)
  local id, kind = action(slot)
  local cursor = __AndroidCursor()
  if cursor then PlaceAction(slot); return end
  if not id then return end
  __AndroidSetCursor({ kind = "action", actionType = kind == ACTION_ITEM and "item" or kind == ACTION_MACRO and "macro" or "spell",
                       id = id, value = bit.bor(id, bit.lshift(kind, 24)) })
  setAction(slot, 0)
end
function PlaceAction(slot)
  local cursor = __AndroidCursor()
  if not cursor then return end
  local value
  if cursor.kind == "spell" then value = cursor.id
  elseif cursor.kind == "item" then value = bit.bor(cursor.entry or 0, bit.lshift(ACTION_ITEM, 24))
  elseif cursor.kind == "action" then value = cursor.value end
  local oldId, oldKind = action(slot)
  if value then setAction(slot, value) end
  if cursor.kind == "action" and oldId then
    __AndroidSetCursor({ kind = "action", actionType = oldKind == ACTION_ITEM and "item" or "spell", id = oldId,
                         value = bit.bor(oldId, bit.lshift(oldKind, 24)) })
  else
    ClearCursor()
  end
end
