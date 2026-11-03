-- Android port: player data changes (UiRuntime::sync) and server notifications to the original
-- script events.
local signal = __AndroidSignal
local Field = __AndroidPlayerField
local last = {}
-- quiet: the first sight of a value (login) is not announced as a change.
local function changed(key, value, quiet)
  local old = last[key]
  last[key] = value
  if old == nil then return not quiet end
  return old ~= value
end
local function region(first, count)
  local parts = {}
  for i = first, first + count - 1 do parts[#parts + 1] = Field(i) or 0 end
  return table.concat(parts, ",")
end
local function bagsUpdated()
  for _, bag in ipairs({0, 1, 2, 3, 4, -2}) do signal("BAG_UPDATE", bag) end
  signal("UNIT_INVENTORY_CHANGED", "player")
  signal("PLAYER_EQUIPMENT_CHANGED", 0, 1)
  signal("ACTIONBAR_UPDATE_COUNT")
end

function __AndroidPlayerDataChanged(...)
  local kinds = {}
  for i = 1, select("#", ...) do kinds[select(i, ...)] = true end
  if kinds.iteminfo then __AndroidItemsInfoChanged() end
  if kinds.spells then
    __AndroidSpellsChanged()
    signal("SPELLS_CHANGED")
    signal("ACTIONBAR_UPDATE_STATE")
  end
  if kinds.actions then
    signal("ACTIONBAR_SLOT_CHANGED", 0)
    signal("UPDATE_BONUS_ACTIONBAR")
  end
  if kinds.party then
    __AndroidPartyChanged()
    signal("PARTY_MEMBERS_CHANGED")
    signal("PARTY_LEADER_CHANGED")
    signal("RAID_ROSTER_UPDATE")
    signal("PARTY_LOOT_METHOD_CHANGED")
  end
  local questsDirty = kinds.quests or kinds.iteminfo
  if kinds.fields then
    if changed("money", Field(0x492)) then signal("PLAYER_MONEY") end
    if changed("xp", region(0x27A, 2)) then signal("PLAYER_XP_UPDATE", "player") end
    if changed("rest", Field(0x491)) then signal("UPDATE_EXHAUSTION") end
    local oldPoints = last.points
    if changed("points", Field(0x3FC), true) then
      -- CHARACTER_POINTS_CHANGED(talent point change, levels gained): no level-up text here.
      signal("CHARACTER_POINTS_CHANGED", (Field(0x3FC) or 0) - (oldPoints or 0), 0)
    end
    if changed("questlog", region(0x9E, 125)) then questsDirty = true end
    if changed("inventory", region(0x144, 236)) then kinds.items = true end
    if changed("stats", region(0x54, 0x40)) then
      signal("UNIT_STATS", "player")
      signal("UNIT_RESISTANCES", "player")
      signal("UNIT_ATTACK_POWER", "player")
      signal("UNIT_ATTACK_SPEED", "player")
      signal("UNIT_DAMAGE", "player")
    end
  end
  if kinds.items or kinds.iteminfo then
    bagsUpdated()
    questsDirty = true
  end
  if questsDirty then
    __AndroidQuestsChanged()
    signal("QUEST_LOG_UPDATE")
    signal("UNIT_QUEST_LOG_CHANGED", "player")
  end
end

local function systemMessage(text)
  signal("CHAT_MSG_SYSTEM", text, "", "", "", "", "", 0, 0, "", 0, 0, "")
end
function __AndroidServerEvent(name, ...)
  local handler = __AndroidCombatHandlers and __AndroidCombatHandlers[name]
  if handler then handler(...); return end
  if name == "__ANDROID_CAST_FAILED" then
    local _, result = ...
    local token = SPELL_FAILED_TOKENS[tonumber(result) or -1]
    local text = token and _G[token]
    if text and text ~= "" then signal("UI_ERROR_MESSAGE", text) end
  elseif name == "__ANDROID_GROUP_DECLINE" then
    systemMessage(format(ERR_DECLINE_GROUP_S or "%s declines your group invitation.", (...)))
  elseif name == "__ANDROID_PARTY_RESULT" then
    local _, who, result = ...
    local messages = { ["1"] = ERR_BAD_PLAYER_NAME_S, ["3"] = ERR_GROUP_FULL, ["4"] = ERR_ALREADY_IN_GROUP_S,
                       ["5"] = ERR_INVITE_PLAYER_S }
    local text = messages[result]
    if text then signal("UI_ERROR_MESSAGE", format(text, who or "")) end
  elseif name == "PARTY_INVITE_REQUEST" then
    signal("PARTY_INVITE_REQUEST", ...)
  else
    signal(name, ...)
  end
end

function __AndroidDataLoginEvents()
  __AndroidItemsInfoChanged()
  __AndroidSpellsChanged()
  __AndroidPartyChanged()
  __AndroidQuestsChanged()
  signal("SPELLS_CHANGED")
  signal("ACTIONBAR_SLOT_CHANGED", 0)
  signal("PARTY_MEMBERS_CHANGED")
  signal("QUEST_LOG_UPDATE")
  signal("PLAYER_MONEY")
  signal("PLAYER_XP_UPDATE", "player")
  bagsUpdated()
end
