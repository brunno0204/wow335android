-- Android port: quest log from PLAYER_QUEST_LOG_1_1 (25 x 5 fields: id, state, 4 x u16 counts,
-- time) and the quest templates (SMSG_QUEST_QUERY_RESPONSE), grouped under zone headers.
local Field = __AndroidPlayerField
local QUEST_LOG = 0x9e
local entries = {}      -- log index -> {header=true,title} or {slot, id, ...}
local selected = 0
local collapsed = {}
local watched = {}      -- quest id -> true (all quests are watched by default)
local unwatched = {}

local function slotInfo(slot)
  local base = QUEST_LOG + (slot - 1) * 5
  local id = Field(base) or 0
  if id == 0 then return end
  local state = Field(base + 1) or 0
  local low, high = Field(base + 2) or 0, Field(base + 3) or 0
  local counts = { low % 65536, floor(low / 65536), high % 65536, floor(high / 65536) }
  return id, state, counts
end
local function headerName(zoneOrSort)
  if zoneOrSort and zoneOrSort > 0 then return __AndroidDbc("AreaTable", zoneOrSort, "areaName") end
  if zoneOrSort and zoneOrSort < 0 then return __AndroidDbc("QuestSort", -zoneOrSort, "sortName") end
end
local function rebuild()
  local groups, order = {}, {}
  for slot = 1, 25 do
    local id, state, counts = slotInfo(slot)
    if id then
      local q = { slot = slot, id = id, state = state, counts = counts }
      local title, level, kind, zoneOrSort, flags, objectives, details, players = __AndroidQuest(id)
      q.title = title or ("Quest #" .. id)
      q.level = level or 0
      q.objectives, q.details, q.players, q.flags = objectives or "", details or "", players or 0, flags or 0
      local header = headerName(zoneOrSort) or (MISCELLANEOUS or "Miscellaneous")
      if not groups[header] then groups[header] = {}; order[#order + 1] = header end
      tinsert(groups[header], q)
      if not unwatched[id] then watched[id] = true end
    end
  end
  sort(order)
  entries = {}
  for _, header in ipairs(order) do
    entries[#entries + 1] = { header = true, title = header }
    if not collapsed[header] then
      for _, q in ipairs(groups[header]) do entries[#entries + 1] = q end
    end
  end
  if selected > #entries then selected = 0 end
end
function __AndroidQuestsChanged() rebuild() end

local function entry(index) return entries[tonumber(index) or selected] end
local function quest(index) local e = entry(index); if e and not e.header then return e end end

function GetNumQuestLogEntries()
  local quests = 0
  for slot = 1, 25 do if slotInfo(slot) then quests = quests + 1 end end
  return #entries, quests
end
function GetQuestLogTitle(index)
  local e = entry(index)
  if not e then return end
  if e.header then return e.title, 0, nil, 0, 1, collapsed[e.title] and 1 or nil, nil, nil, nil end
  local complete = nil
  if bit.band(e.state, 2) ~= 0 then complete = -1 elseif bit.band(e.state, 1) ~= 0 then complete = 1 end
  local tag = (e.players or 0) > 1 and GROUP or nil
  return e.title, e.level, tag, e.players, nil, nil, complete, nil, e.id
end
function GetQuestLogSelection() return selected end
function SelectQuestLogEntry(index) selected = tonumber(index) or 0 end
function ExpandQuestHeader(index)
  if index == 0 then collapsed = {} else local e = entry(index); if e and e.header then collapsed[e.title] = nil end end
  rebuild(); __AndroidSignal("QUEST_LOG_UPDATE")
end
function CollapseQuestHeader(index)
  if index == 0 then for _, e in ipairs(entries) do if e.header then collapsed[e.title] = true end end
  else local e = entry(index); if e and e.header then collapsed[e.title] = true end end
  rebuild(); __AndroidSignal("QUEST_LOG_UPDATE")
end
function GetQuestLogQuestText(index)
  local q = quest(index)
  if q then return q.details, q.objectives end
end
function GetQuestLogIndexByID(id)
  for i, e in ipairs(entries) do if e.id == id then return i end end
  return 0
end
function GetQuestLink(index)
  local q = quest(index)
  if q then return format("|cffffff00|Hquest:%d:%d|h[%s]|h|r", q.id, q.level, q.title) end
end
function GetQuestLogPushable() return nil end
function GetQuestLogTimeLeft() return nil end
function GetQuestLogGroupNum(index) local q = quest(index); return q and q.players or 0 end
function GetQuestLogRequiredMoney() return 0 end
function GetQuestLogRewardMoney() return 0 end
function GetNumQuestLogRewards() return 0 end
function GetNumQuestLogChoices() return 0 end
function GetQuestLogRewardSpell() return nil end
function GetQuestLogRewardTalents() return 0 end
function GetQuestLogRewardTitle() return nil end
function GetQuestLogRewardHonor() return 0 end
function GetQuestLogRewardXP() return 0 end
function IsCurrentQuestFailed() local q = quest(); if q and bit.band(q.state, 2) ~= 0 then return 1 end end
function GetQuestLogSpecialItemInfo() return nil end
function GetQuestDifficultyColor(level)
  local diff = (tonumber(level) or 0) - (UnitLevel("player") or 0)
  if diff >= 5 then return QuestDifficultyColors and QuestDifficultyColors["impossible"] or {r = 1, g = 0.1, b = 0.1}
  elseif diff >= 3 then return QuestDifficultyColors and QuestDifficultyColors["verydifficult"] or {r = 1, g = 0.5, b = 0.25}
  elseif diff >= -2 then return QuestDifficultyColors and QuestDifficultyColors["difficult"] or {r = 1, g = 1, b = 0}
  elseif diff >= -7 then return QuestDifficultyColors and QuestDifficultyColors["standard"] or {r = 0.25, g = 0.75, b = 0.25}
  end
  return QuestDifficultyColors and QuestDifficultyColors["trivial"] or {r = 0.5, g = 0.5, b = 0.5}
end

-- Objectives ("leader boards"): kill/object counts from the log fields, items from the bags.
local function objectives(q)
  local list = {}
  local values = { __AndroidQuest(q.id) }
  -- values: 10 scalars, then 4 x (target, count, text), then 6 x (item, count).
  for i = 0, 3 do
    local target, need, text = values[11 + i * 3], values[12 + i * 3], values[13 + i * 3]
    if target and target ~= 0 and need and need > 0 then
      local have = min(q.counts[i + 1] or 0, need)
      local line
      if text and text ~= "" then
        line = format(QUEST_OBJECTS_FOUND or "%s: %d/%d", text, have, need)
      elseif target > 0 then
        line = format(QUEST_MONSTERS_KILLED or "%s slain: %d/%d", __AndroidObjectiveName(target) or "?", have, need)
      else
        line = format(QUEST_OBJECTS_FOUND or "%s: %d/%d", __AndroidObjectiveName(target) or "?", have, need)
      end
      list[#list + 1] = { text = line, kind = target > 0 and "monster" or "object", done = have >= need }
    end
  end
  for i = 0, 5 do
    local item, need = values[23 + i * 2], values[24 + i * 2]
    if item and item > 0 and need and need > 0 then
      local have = min(GetItemCount(item), need)
      local name = GetItemInfo(item) or "?"
      list[#list + 1] = { text = format(QUEST_ITEMS_NEEDED or "%s: %d/%d", name, have, need), kind = "item",
                          done = have >= need }
    end
  end
  return list
end
function GetNumQuestLeaderBoards(index)
  local q = quest(index)
  return q and #objectives(q) or 0
end
function GetQuestLogLeaderBoard(n, index)
  local q = quest(index)
  local o = q and objectives(q)[tonumber(n) or 0]
  if o then return o.text, o.kind, o.done and 1 or nil end
end
function GetQuestLogCompletionText(index) local q = quest(index); return q and select(9, __AndroidQuest(q.id)) end

-- Watch list (WatchFrame): every quest unless removed.
local function watchList()
  local list = {}
  for i, e in ipairs(entries) do if not e.header and watched[e.id] then list[#list + 1] = i end end
  return list
end
function GetNumQuestWatches() return #watchList() end
function GetQuestIndexForWatch(n) return watchList()[tonumber(n) or 0] end
function IsQuestWatched(index) local q = quest(index); if q and watched[q.id] then return 1 end end
function AddQuestWatch(index) local q = quest(index); if q then watched[q.id] = true; unwatched[q.id] = nil end end
function RemoveQuestWatch(index) local q = quest(index); if q then watched[q.id] = nil; unwatched[q.id] = true end end
function GetQuestWatchType() return 0 end
function GetNumTrackedAchievements() return 0 end
function GetTrackedAchievements() return end
function GetQuestLogIsAutoComplete() return nil end

-- Abandon (CMSG_QUEST_LOG_REMOVE_QUEST with the log slot).
local abandon
function SetAbandonQuest() abandon = quest() end
function GetAbandonQuestName() return abandon and abandon.title end
function GetAbandonQuestItems() return nil end
function AbandonQuest()
  if abandon then __AndroidSend(OP.CMSG_QUEST_LOG_REMOVE_QUEST, "b", abandon.slot - 1) end
  abandon = nil
end
