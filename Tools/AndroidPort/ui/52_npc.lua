-- Android port: loot window, NPC gossip, quest giver dialogs, vendors and trainers from the
-- server packets (ServerEvents.cpp) with the original API; replies go out with __AndroidSend.
local signal = __AndroidSignal
local Dbc = __AndroidDbc
local H = __AndroidCombatHandlers

local npc               -- GUID of the NPC the open dialog belongs to (unit "npc")
local function setNpc(guid) npc = guid end
local previousToken = __AndroidUnitToken
function __AndroidUnitToken(token)
  if token == "npc" or token == "questnpc" then return npc end
  if previousToken then return previousToken(token) end
end
function InteractUnit(unit) local guid = UnitGUID(unit); if guid then __AndroidInteract(guid) end end
function ForceGossip() return nil end

-- NPC/quest text tokens the client substitutes: $N name, $R race, $C class, $Gmale:female; and $B.
local function subst(text)
  if not text or text == "" then return text or "" end
  local female = UnitSex("player") == 3
  text = gsub(text, "%$[Gg]%s*([^:;]*):([^;]*);", function(male, fem) return female and fem or male end)
  text = gsub(text, "%$[Nn]", UnitName("player") or "")
  text = gsub(text, "%$[Rr]", (UnitRace("player")) or "")
  text = gsub(text, "%$[Cc]", (UnitClass("player")) or "")
  text = gsub(text, "%$[Bb]", "\n")
  return text
end
__AndroidQuestText = subst
local function itemValues(entry, count)
  local name, _, quality, _, _, _, _, _, _, icon = GetItemInfo(entry)
  return name or RETRIEVING_ITEM_INFO or "Retrieving item information", icon or "Interface\\Icons\\INV_Misc_QuestionMark",
         count or 1, quality or 1, 1
end

function GetCoinText(amount, separator)
  amount = floor(tonumber(amount) or 0)
  separator = separator or ", "
  local gold, silver, copper = floor(amount / 10000), floor(amount / 100) % 100, amount % 100
  local parts = {}
  if gold > 0 then parts[#parts + 1] = format(GOLD_AMOUNT or "%d Gold", gold) end
  if silver > 0 then parts[#parts + 1] = format(SILVER_AMOUNT or "%d Silver", silver) end
  if copper > 0 or #parts == 0 then parts[#parts + 1] = format(COPPER_AMOUNT or "%d Copper", copper) end
  return table.concat(parts, separator)
end

-- ---- Loot (SMSG_LOOT_RESPONSE ...): UI slot 1 is the money when there is any.
local loot
H.__ANDROID_LOOT = function(guid, kind, gold, count, ...)
  local v = { ... }
  loot = { guid = guid, gold = tonumber(gold) or 0, slots = {} }
  if loot.gold > 0 then loot.slots[1] = { coin = true } end
  for i = 1, (tonumber(count) or 0) * 4, 4 do
    local entry = tonumber(v[i + 1])
    GetItemInfo(entry)
    loot.slots[#loot.slots + 1] = { server = tonumber(v[i]), entry = entry, count = tonumber(v[i + 2]) or 1,
                                    locked = tonumber(v[i + 3]) == 3 }
  end
  signal("LOOT_OPENED", 0)
end
H.__ANDROID_LOOT_ERROR = function(guid, code)
  signal("UI_ERROR_MESSAGE", ERR_LOOT_DIDNT_KILL or "You can't loot that.")
end
H.__ANDROID_LOOT_REMOVED = function(server)
  if not loot then return end
  server = tonumber(server)
  for i, s in pairs(loot.slots) do
    if s.server == server and not s.gone then s.gone = true; signal("LOOT_SLOT_CLEARED", i) end
  end
end
H.__ANDROID_LOOT_CLEAR_MONEY = function()
  if loot and loot.slots[1] and loot.slots[1].coin and not loot.slots[1].gone then
    loot.slots[1].gone = true
    signal("LOOT_SLOT_CLEARED", 1)
  end
end
H.LOOT_CLOSED = function()
  loot = nil
  signal("LOOT_CLOSED")
end
function GetNumLootItems() return loot and #loot.slots or 0 end
function LootSlotIsCoin(slot) local s = loot and loot.slots[slot]; if s and s.coin and not s.gone then return 1 end end
function LootSlotIsItem(slot) local s = loot and loot.slots[slot]; if s and s.entry and not s.gone then return 1 end end
function GetLootSlotInfo(slot)
  local s = loot and loot.slots[slot]
  if not s or s.gone then return end
  if s.coin then
    local text = gsub(GetCoinText(loot.gold, "\n"), ",", "")
    return GetCoinIcon(loot.gold), text, 0, 0, nil
  end
  local name, icon, count, quality = itemValues(s.entry, s.count)
  return icon, name, count, quality, s.locked and 1 or nil
end
function GetLootSlotLink(slot)
  local s = loot and loot.slots[slot]
  if s and s.entry then local _, link = GetItemInfo(s.entry); return link end
end
function LootSlot(slot)
  local s = loot and loot.slots[slot]
  if not s or s.gone then return end
  if s.coin then __AndroidSend(OP.CMSG_LOOT_MONEY, "")
  else __AndroidSend(OP.CMSG_AUTOSTORE_LOOT_ITEM, "b", s.server) end
end
function ConfirmLootSlot(slot) LootSlot(slot) end
function CloseLoot()
  if loot then __AndroidSend(OP.CMSG_LOOT_RELEASE, "l", loot.guid) end
end
function IsFishingLoot() return nil end
function GetLootThreshold() return 2 end
function GetLootMethod() return "freeforall", nil, nil end
function GetCoinIcon(amount)
  amount = tonumber(amount) or 0
  if amount < 100 then return "Interface\\Icons\\INV_Misc_Coin_06"
  elseif amount < 10000 then return "Interface\\Icons\\INV_Misc_Coin_04" end
  return "Interface\\Icons\\INV_Misc_Coin_02"
end

-- ---- Gossip (SMSG_GOSSIP_MESSAGE + SMSG_NPC_TEXT_UPDATE).
local gossip
local npcTexts = {}
local GOSSIP_TYPES = { [0] = "gossip", [1] = "vendor", [2] = "taxi", [3] = "trainer", [4] = "healer", [5] = "binder",
                       [6] = "banker", [7] = "petition", [8] = "tabard", [9] = "battlemaster", [10] = "gossip" }
local function questIsComplete(id)
  for slot = 1, 25 do
    local base = 0x9e + (slot - 1) * 5
    if __AndroidPlayerField(base) == id then return bit.band(__AndroidPlayerField(base + 1) or 0, 1) ~= 0 end
  end
end
local function questLevelTrivial(level)
  return (tonumber(level) or 0) > 0 and (tonumber(level) or 0) < UnitLevel("player") - 7
end
H.__ANDROID_GOSSIP = function(guid, menu, text, options, ...)
  local v = { ... }
  setNpc(guid)
  gossip = { guid = guid, menu = tonumber(menu), text = tonumber(text), options = {}, available = {}, active = {} }
  local i = 1
  for _ = 1, tonumber(options) or 0 do
    gossip.options[#gossip.options + 1] = { index = tonumber(v[i]), icon = tonumber(v[i + 1]), coded = v[i + 2] ~= "0",
                                            text = v[i + 3] }
    i = i + 4
  end
  local quests = tonumber(v[i]) or 0
  i = i + 1
  for _ = 1, quests do
    local q = { id = tonumber(v[i]), icon = tonumber(v[i + 1]), level = tonumber(v[i + 2]), title = v[i + 3] }
    if q.icon == 2 or q.icon == 8 then gossip.available[#gossip.available + 1] = q
    else gossip.active[#gossip.active + 1] = q end
    i = i + 4
  end
  signal("GOSSIP_SHOW")
end
H.__ANDROID_NPC_TEXT = function(id, text)
  npcTexts[tonumber(id)] = text
  if gossip and gossip.text == tonumber(id) and GossipFrame and GossipFrame:IsShown() then
    GossipGreetingText:SetText(subst(text))
  end
end
H.GOSSIP_CLOSED = function() gossip = nil; signal("GOSSIP_CLOSED") end
function GetGossipText()
  local text = gossip and npcTexts[gossip.text or 0]
  return subst(text or "")
end
function GetNumGossipOptions() return gossip and #gossip.options or 0 end
function GetNumGossipAvailableQuests() return gossip and #gossip.available or 0 end
function GetNumGossipActiveQuests() return gossip and #gossip.active or 0 end
function GetGossipOptions()
  if not gossip then return end
  local out = {}
  for _, o in ipairs(gossip.options) do out[#out + 1] = o.text; out[#out + 1] = GOSSIP_TYPES[o.icon] or "gossip" end
  return unpack(out)
end
function GetGossipAvailableQuests()
  if not gossip then return end
  local out = {}
  for i, q in ipairs(gossip.available) do
    local n = (i - 1) * 5
    out[n + 1] = q.title; out[n + 2] = q.level; out[n + 3] = questLevelTrivial(q.level) and 1 or nil
  end
  return unpack(out, 1, #gossip.available * 5)
end
function GetGossipActiveQuests()
  if not gossip then return end
  local out = {}
  for i, q in ipairs(gossip.active) do
    local n = (i - 1) * 4
    out[n + 1] = q.title; out[n + 2] = q.level; out[n + 3] = questLevelTrivial(q.level) and 1 or nil
    out[n + 4] = questIsComplete(q.id) and 1 or nil
  end
  return unpack(out, 1, #gossip.active * 4)
end
function SelectGossipOption(index, text)
  local o = gossip and gossip.options[tonumber(index) or 0]
  if not o then return end
  if o.coded then __AndroidSend(OP.CMSG_GOSSIP_SELECT_OPTION, "liis", gossip.guid, gossip.menu, o.index, text or "")
  else __AndroidSend(OP.CMSG_GOSSIP_SELECT_OPTION, "lii", gossip.guid, gossip.menu, o.index) end
end
function SelectGossipAvailableQuest(index)
  local q = gossip and gossip.available[tonumber(index) or 0]
  if q then __AndroidSend(OP.CMSG_QUEST_GIVER_QUERY_QUEST, "lib", gossip.guid, q.id, 1) end
end
function SelectGossipActiveQuest(index)
  local q = gossip and gossip.active[tonumber(index) or 0]
  if q then __AndroidSend(OP.CMSG_QUEST_GIVER_COMPLETE_QUEST, "li", gossip.guid, q.id) end
end
function CloseGossip()
  if gossip then gossip = nil; signal("GOSSIP_CLOSED") end
end

-- ---- Quest giver dialogs.
local greeting, quest
H.__ANDROID_QUEST_LIST = function(guid, text, count, ...)
  local v = { ... }
  setNpc(guid)
  greeting = { guid = guid, text = text, available = {}, active = {} }
  local i = 1
  for _ = 1, tonumber(count) or 0 do
    local q = { id = tonumber(v[i]), icon = tonumber(v[i + 1]), level = tonumber(v[i + 2]), title = v[i + 3] }
    if q.icon == 2 or q.icon == 8 then greeting.available[#greeting.available + 1] = q
    else greeting.active[#greeting.active + 1] = q end
    i = i + 4
  end
  signal("QUEST_GREETING")
end
local function rewardsFrom(list, start)
  local count = tonumber(list[start]) or 0
  local items = {}
  for k = 1, count do
    local entry = tonumber(list[start + (k - 1) * 2 + 1])
    GetItemInfo(entry)
    items[k] = { entry = entry, count = tonumber(list[start + (k - 1) * 2 + 2]) or 1 }
  end
  return items, start + 1 + count * 2
end
H.__ANDROID_QUEST_DETAIL = function(guid, id, title, details, objectives, money, ...)
  local v = { ... }
  setNpc(guid)
  local choices, next = rewardsFrom(v, 1)
  local rewards = rewardsFrom(v, next)
  quest = { guid = guid, id = tonumber(id), title = title, text = details, objectives = objectives,
            money = tonumber(money) or 0, choices = choices, rewards = rewards, required = {} }
  signal("QUEST_DETAIL")
end
H.__ANDROID_QUEST_PROGRESS = function(guid, id, title, text, money, items, ...)
  local v = { ... }
  setNpc(guid)
  local required = {}
  for k = 1, tonumber(items) or 0 do
    local entry = tonumber(v[(k - 1) * 2 + 1])
    GetItemInfo(entry)
    required[k] = { entry = entry, count = tonumber(v[(k - 1) * 2 + 2]) or 1 }
  end
  quest = { guid = guid, id = tonumber(id), title = title, progress = text, moneyToGet = tonumber(money) or 0,
            required = required, completable = tonumber(v[(tonumber(items) or 0) * 2 + 1] or 0) ~= 0,
            choices = {}, rewards = {} }
  signal("QUEST_PROGRESS")
end
H.__ANDROID_QUEST_COMPLETE = function(guid, id, title, text, money, xp, ...)
  local v = { ... }
  setNpc(guid)
  local choices, next = rewardsFrom(v, 1)
  local rewards = rewardsFrom(v, next)
  quest = { guid = guid, id = tonumber(id), title = title, reward = text, money = tonumber(money) or 0,
            xp = tonumber(xp) or 0, choices = choices, rewards = rewards, required = {} }
  signal("QUEST_COMPLETE")
end
H.__ANDROID_QUEST_DONE = function(id, xp, money)
  local title = __AndroidQuest(tonumber(id)) or (quest and quest.title) or ""
  local chat = function(text) signal("CHAT_MSG_SYSTEM", text, "", "", "", "", "", 0, 0, "", 0, 0, "") end
  chat(format(ERR_QUEST_COMPLETE_S or "%s completed.", title))
  if (tonumber(xp) or 0) > 0 then chat(format(ERR_QUEST_REWARD_EXP_I or "Experience gained: %d.", tonumber(xp))) end
  if (tonumber(money) or 0) > 0 then chat(format(ERR_QUEST_REWARD_MONEY_S or "Received %s.", GetCoinText(tonumber(money), ", "))) end
  quest = nil
  signal("QUEST_FINISHED")
  signal("QUEST_LOG_UPDATE")
end
H.__ANDROID_QUEST_INVALID = function(reason)
  signal("UI_ERROR_MESSAGE", ERR_QUEST_FAILED_LOW_LEVEL or "You are not eligible for that quest.")
end
H.__ANDROID_QUEST_FAILED = function(id)
  signal("UI_ERROR_MESSAGE", format(ERR_QUEST_FAILED_S or "%s failed.", __AndroidQuest(tonumber(id)) or ""))
end
H.__ANDROID_QUEST_LOG_FULL = function() signal("UI_ERROR_MESSAGE", ERR_QUEST_LOG_FULL or "Your quest log is full.") end
H.__ANDROID_QUEST_OBJECTIVES_COMPLETE = function(id)
  signal("UI_INFO_MESSAGE", format(ERR_QUEST_OBJECTIVE_COMPLETE_S or "%s (Complete)", __AndroidQuest(tonumber(id)) or ""))
end
H.__ANDROID_QUEST_KILL = function(id, entry, count, required)
  local name = __AndroidObjectiveName(tonumber(entry) or 0) or "?"
  signal("UI_INFO_MESSAGE", format(QUEST_MONSTERS_KILLED or "%s slain: %d/%d", name, tonumber(count) or 0, tonumber(required) or 0))
end
local confirm
H.__ANDROID_QUEST_ACCEPT_CONFIRM = function(id, title, guid, name)
  confirm = tonumber(id)
  signal("QUEST_ACCEPT_CONFIRM", name, title)
end
function ConfirmAcceptQuest() if confirm then __AndroidSend(OP.CMSG_QUEST_CONFIRM_ACCEPT, "i", confirm) end end

function GetGreetingText() return subst(greeting and greeting.text) end
function GetNumActiveQuests() return greeting and #greeting.active or 0 end
function GetNumAvailableQuests() return greeting and #greeting.available or 0 end
function GetActiveTitle(i) local q = greeting and greeting.active[i]; if q then return q.title, questIsComplete(q.id) and 1 or nil end end
function GetAvailableTitle(i) local q = greeting and greeting.available[i]; return q and q.title end
function GetActiveLevel(i) local q = greeting and greeting.active[i]; return q and q.level or 0 end
function GetAvailableLevel(i) local q = greeting and greeting.available[i]; return q and q.level or 0 end
function IsActiveQuestTrivial(i) local q = greeting and greeting.active[i]; if q and questLevelTrivial(q.level) then return 1 end end
function GetAvailableQuestInfo(i)
  local q = greeting and greeting.available[i]
  if q then return questLevelTrivial(q.level) and 1 or nil, nil, nil end
end
function SelectActiveQuest(i)
  local q = greeting and greeting.active[i]
  if q then __AndroidSend(OP.CMSG_QUEST_GIVER_COMPLETE_QUEST, "li", greeting.guid, q.id) end
end
function SelectAvailableQuest(i)
  local q = greeting and greeting.available[i]
  if q then __AndroidSend(OP.CMSG_QUEST_GIVER_QUERY_QUEST, "lib", greeting.guid, q.id, 1) end
end
function GetTitleText() return quest and quest.title or "" end
function GetQuestText() return subst(quest and quest.text) end
function GetObjectiveText() return subst(quest and quest.objectives) end
function GetProgressText() return subst(quest and quest.progress) end
function GetRewardText() return subst(quest and quest.reward) end
function GetQuestBackgroundMaterial() return nil end
function QuestGetAutoAccept() return nil end
function QuestFlagsPVP() return nil end
function GetSuggestedGroupNum() return 0 end
function IsQuestCompletable() if quest and quest.completable then return 1 end end
function GetQuestMoneyToGet() return quest and quest.moneyToGet or 0 end
function GetNumQuestItems() return quest and #quest.required or 0 end
function GetNumQuestChoices() return quest and #quest.choices or 0 end
function GetNumQuestRewards() return quest and #quest.rewards or 0 end
function GetRewardMoney() return quest and quest.money or 0 end
function GetRewardXP() return quest and quest.xp or 0 end
function GetRewardSpell() return nil end
function GetRewardTitle() return nil end
function GetRewardTalents() return 0 end
function GetRewardHonor() return 0 end
function GetRewardArenaPoints() return 0 end
function GetNumRewardCurrencies() return 0 end
local function questItem(kind, i)
  if not quest then return end
  local list = kind == "choice" and quest.choices or kind == "reward" and quest.rewards or quest.required
  return list[tonumber(i) or 0]
end
function GetQuestItemInfo(kind, i)
  local it = questItem(kind, i)
  if it then return itemValues(it.entry, it.count) end
end
function GetQuestItemLink(kind, i)
  local it = questItem(kind, i)
  if it then local _, link = GetItemInfo(it.entry); return link end
end
function AcceptQuest() if quest then __AndroidSend(OP.CMSG_QUEST_GIVER_ACCEPT_QUEST, "lii", quest.guid, quest.id, 0) end end
function DeclineQuest() __AndroidSend(OP.CMSG_QUEST_GIVER_CANCEL, ""); quest = nil; signal("QUEST_FINISHED") end
function CloseQuest() __AndroidSend(OP.CMSG_QUEST_GIVER_CANCEL, ""); quest = nil; greeting = nil; signal("QUEST_FINISHED") end
function CompleteQuest() if quest then __AndroidSend(OP.CMSG_QUEST_GIVER_REQUEST_REWARD, "li", quest.guid, quest.id) end end
function GetQuestReward(choice)
  if quest then __AndroidSend(OP.CMSG_QUEST_GIVER_CHOOSE_REWARD, "lii", quest.guid, quest.id, math.max(0, (tonumber(choice) or 1) - 1)) end
end
function QuestChooseRewardError() signal("UI_ERROR_MESSAGE", ERR_QUEST_MUST_CHOOSE or "You must choose a reward.") end

-- ---- Vendor (SMSG_LIST_INVENTORY).
local merchant
H.__ANDROID_VENDOR = function(guid, count, ...)
  local v = { ... }
  setNpc(guid)
  local items = {}
  for k = 1, tonumber(count) or 0 do
    local b = (k - 1) * 7
    local entry = tonumber(v[b + 2])
    GetItemInfo(entry)
    items[k] = { slot = tonumber(v[b + 1]), entry = entry, available = tonumber(v[b + 3]) or -1,
                 price = tonumber(v[b + 4]) or 0, stack = tonumber(v[b + 6]) or 1, extended = tonumber(v[b + 7]) or 0 }
  end
  local open = merchant ~= nil
  merchant = { guid = guid, items = items }
  if open then signal("MERCHANT_UPDATE") else signal("MERCHANT_SHOW") end
end
H.__ANDROID_VENDOR_ERROR = function(kind, code)
  local sell = { [1] = ERR_VENDOR_CANT_FIND_ITEM, [2] = ERR_VENDOR_HATES_YOU, [4] = ERR_VENDOR_NOT_INTERESTED }
  local buy = { [0] = ERR_VENDOR_CANT_FIND_ITEM, [1] = ERR_ITEM_SOLD_OUT, [2] = ERR_NOT_ENOUGH_MONEY,
                [4] = ERR_VENDOR_HATES_YOU, [5] = ERR_VENDOR_TOO_FAR, [6] = ERR_ITEM_SOLD_OUT, [8] = ERR_CANT_CARRY_MORE_OF_THIS }
  local text = (kind == "sell" and sell or buy)[tonumber(code) or -1]
  signal("UI_ERROR_MESSAGE", text or ERR_VENDOR_NOT_INTERESTED or "The merchant doesn't want that item.")
end
function __AndroidMerchantOpen() return merchant ~= nil end
function GetMerchantNumItems() return merchant and #merchant.items or 0 end
function GetMerchantItemInfo(i)
  local it = merchant and merchant.items[tonumber(i) or 0]
  if not it then return end
  local name, icon = itemValues(it.entry)
  local usable = 1
  local t = __AndroidItemTemplateTable(it.entry)
  if t and (t.reqLevel or 0) > UnitLevel("player") then usable = nil end
  return name, icon, it.price, it.stack, it.available, usable, it.extended > 0 and 1 or nil
end
function GetMerchantItemLink(i)
  local it = merchant and merchant.items[tonumber(i) or 0]
  if it then local _, link = GetItemInfo(it.entry); return link end
end
function GetMerchantItemMaxStack(i)
  local it = merchant and merchant.items[tonumber(i) or 0]
  local t = it and __AndroidItemTemplateTable(it.entry)
  return t and t.maxStack or 1
end
function GetMerchantItemCostInfo() return 0, 0, 0 end
function GetMerchantItemCostItem() return nil end
function BuyMerchantItem(i, quantity)
  local it = merchant and merchant.items[tonumber(i) or 0]
  if not it then return end
  local count = tonumber(quantity) or it.stack
  __AndroidSend(OP.CMSG_BUY_ITEM, "liiib", merchant.guid, it.entry, it.slot, count, 0)
end
function PickupMerchantItem(i) BuyMerchantItem(i) end
function CloseMerchant() if merchant then merchant = nil; signal("MERCHANT_CLOSED") end end
function CanMerchantRepair()
  local u = merchant and { __AndroidUnit(merchant.guid) }
  if u and u[1] and bit.band(u[16] or 0, 0x1000) ~= 0 then return 1 end
end
function CanGuildBankRepair() return nil end
function GetRepairAllCost()
  -- Durability lost x the item's repair price (DurabilityCosts/DurabilityQuality).
  local total = 0
  for slot = 1, 19 do
    local current, maximum = GetInventoryItemDurability(slot)
    if current and maximum and maximum > current then
      local entry = GetInventoryItemID("player", slot)
      local t = entry and __AndroidItemTemplateTable(entry)
      if t then
        local field = (t.class == 2 and "weaponSubClassCost" .. (t.subclass + 1))
                   or (t.class == 4 and "armorSubClassCost" .. (t.subclass + 1))
        local multiplier = field and Dbc("DurabilityCosts", t.ilevel or 0, field) or 0
        local quality = Dbc("DurabilityQuality", ((t.quality or 1) + 1) * 2, "data") or 1
        total = total + floor((maximum - current) * (multiplier or 1) * quality)
      end
    end
  end
  return total, total > 0 and 1 or nil
end
function RepairAllItems(guildBank)
  if merchant then __AndroidSend(OP.CMSG_REPAIR_ITEM, "llb", merchant.guid, "0x0000000000000000", guildBank and 1 or 0) end
end
function InRepairMode() return nil end
function ShowRepairCursor() RepairAllItems() end
function HideRepairCursor() end
function GetNumBuybackItems() return 0 end
function GetBuybackItemInfo() return nil end
function GetBuybackItemLink() return nil end
function BuybackItem() end
-- Selling: using a bag item while the merchant window is open sells it (CMSG_SELL_ITEM).
local useContainerItem = UseContainerItem
function UseContainerItem(bag, slot, ...)
  if merchant then
    local guid = __AndroidItemLocation(bag, slot)
    if guid then
      local _, count = GetContainerItemInfo(bag, slot)
      __AndroidSend(OP.CMSG_SELL_ITEM, "lli", merchant.guid, guid, count or 1)
    end
    return
  end
  return useContainerItem(bag, slot, ...)
end

-- ---- Trainer (SMSG_TRAINER_LIST), for Blizzard_TrainerUI.
local trainer
local selected = 0
local STATES = { [0] = "available", [1] = "unavailable", [2] = "used" }
local filters = { available = 1, unavailable = 1, used = nil }
local function visible()
  local list = {}
  if not trainer then return list end
  for _, s in ipairs(trainer.spells) do
    if filters[STATES[s.state] or "unavailable"] then list[#list + 1] = s end
  end
  return list
end
H.__ANDROID_TRAINER = function(guid, kind, count, ...)
  local v = { ... }
  setNpc(guid)
  local spells = {}
  for k = 1, tonumber(count) or 0 do
    local b = (k - 1) * 6
    spells[k] = { spell = tonumber(v[b + 1]), state = tonumber(v[b + 2]) or 1, cost = tonumber(v[b + 3]) or 0,
                  level = tonumber(v[b + 4]) or 0, skill = tonumber(v[b + 5]) or 0, skillValue = tonumber(v[b + 6]) or 0 }
  end
  trainer = { guid = guid, kind = tonumber(kind) or 0, spells = spells, greeting = v[(tonumber(count) or 0) * 6 + 1] or "" }
  selected = 0
  if not IsAddOnLoaded("Blizzard_TrainerUI") then LoadAddOn("Blizzard_TrainerUI") end
  signal("TRAINER_SHOW")
  signal("TRAINER_UPDATE")
end
H.__ANDROID_TRAINER_BOUGHT = function(guid, spell)
  if trainer then
    for _, s in ipairs(trainer.spells) do if s.spell == tonumber(spell) then s.state = 2 end end
  end
  signal("TRAINER_UPDATE")
end
local function service(i) return visible()[tonumber(i) or 0] end
function GetNumTrainerServices() return #visible() end
function GetTrainerServiceInfo(i)
  local s = service(i)
  if not s then return end
  local name, rank = __AndroidSpellData(s.spell) and __AndroidSpellData(s.spell).name, __AndroidSpellData(s.spell) and __AndroidSpellData(s.spell).rank
  return name or ("#" .. s.spell), rank or "", STATES[s.state] or "unavailable", nil
end
function GetTrainerServiceIcon(i) local s = service(i); return s and GetSpellTexture(s.spell) end
function GetTrainerServiceCost(i) local s = service(i); return s and s.cost or 0, 0, nil end
function GetTrainerServiceLevelReq(i) local s = service(i); return s and s.level or 0 end
function GetTrainerServiceSkillReq(i)
  local s = service(i)
  if s and s.skill > 0 then return Dbc("SkillLine", s.skill, "displayName"), s.skillValue, 1 end
end
function GetTrainerServiceAbilityReq() return 0 end
function GetTrainerServiceNumAbilityReq() return 0 end
function GetTrainerServiceDescription(i) local s = service(i); return s and GetSpellDescription(s.spell) or "" end
function GetTrainerServiceSkillLine() return nil end
function GetTrainerServiceTypeFilter(kind) return filters[kind] end
function SetTrainerServiceTypeFilter(kind, on) filters[kind] = on and on ~= 0 and 1 or nil; signal("TRAINER_UPDATE") end
function GetTrainerGreetingText() return trainer and trainer.greeting or "" end
function GetTrainerSelectionIndex() return selected end
function SelectTrainerService(i) selected = tonumber(i) or 0 end
function BuyTrainerService(i)
  local s = service(i)
  if s and trainer then __AndroidSend(OP.CMSG_TRAINER_BUY_SPELL, "li", trainer.guid, s.spell) end
end
function IsTradeskillTrainer() if trainer and trainer.kind == 2 then return 1 end end
function IsTrainerServiceSkillStep() return nil end
function IsTrainerServiceLearnSpell() return nil end
function ExpandTrainerSkillLine() end
function CollapseTrainerSkillLine() end
function GetTrainerSkillLines() return nil end
function GetTrainerSkillLineFilter() return 1 end
function SetTrainerSkillLineFilter() end
function GetTrainerServiceStepIndex() return 0 end
function GetTrainerServiceStepReq() return nil end
function CloseTrainer() if trainer then trainer = nil; signal("TRAINER_CLOSED") end end

-- Item templates arriving after a window opened (SMSG_ITEM_QUERY_SINGLE_RESPONSE): refresh the
-- names and icons the loot, merchant and quest windows showed as "Retrieving item information".
local itemsChanged = __AndroidItemsInfoChanged
function __AndroidItemsInfoChanged(...)
  if itemsChanged then itemsChanged(...) end
  if loot then
    for i in pairs(loot.slots) do signal("LOOT_SLOT_CHANGED", i) end
    if LootFrame and LootFrame:IsShown() and LootFrame_Update then LootFrame_Update() end
  end
  if merchant then signal("MERCHANT_UPDATE") end
  if quest then signal("QUEST_ITEM_UPDATE") end
end
function __AndroidTrainerSpell(i) local s = service(i); return s and s.spell end
