-- Android port: Blizzard addons from the archives (Interface\AddOns\<name>\<name>.toc), loaded
-- like WoW/Source/UIUtil/AddOns.cpp: dependencies first, FrameXML_CreateFrames, ADDON_LOADED.
local BLIZZARD = {
  "Blizzard_AchievementUI", "Blizzard_ArenaUI", "Blizzard_AuctionUI", "Blizzard_BarbershopUI",
  "Blizzard_BattlefieldMinimap", "Blizzard_BindingUI", "Blizzard_Calendar", "Blizzard_CombatLog",
  "Blizzard_CombatText", "Blizzard_DebugTools", "Blizzard_GlyphUI", "Blizzard_GMChatUI",
  "Blizzard_GMSurveyUI", "Blizzard_GuildBankUI", "Blizzard_InspectUI", "Blizzard_ItemSocketingUI",
  "Blizzard_MacroUI", "Blizzard_RaidUI", "Blizzard_TalentUI", "Blizzard_TimeManager",
  "Blizzard_TokenUI", "Blizzard_TradeSkillUI", "Blizzard_TrainerUI",
}
local known, info, loaded = {}, {}, {}
for _, name in ipairs(BLIZZARD) do known[strlower(name)] = name end

local function tocPath(name) return "Interface\\AddOns\\" .. name .. "\\" .. name .. ".toc" end
local function metadata(name)
  local real = known[strlower(name)] or name
  if info[real] ~= nil then return info[real] or nil end
  local text = __AndroidReadFile(tocPath(real))
  if not text then info[real] = false; return nil end
  local data = { name = real, fields = {}, deps = {} }
  for line in gmatch(text, "[^\r\n]+") do
    local key, value = strmatch(line, "^##%s*([^:]+):%s*(.-)%s*$")
    if key then data.fields[strlower(key)] = value end
  end
  local deps = data.fields["dependencies"] or data.fields["requireddeps"] or data.fields["dep"]
  if deps then for dep in gmatch(deps, "[^,%s]+") do data.deps[#data.deps + 1] = dep end end
  data.loadOnDemand = data.fields["loadondemand"] == "1"
  info[real] = data
  return data
end

function LoadAddOn(name)
  if type(name) == "number" then name = BLIZZARD[name] end
  if type(name) ~= "string" then return nil, "MISSING" end
  local data = metadata(name)
  if not data then return nil, "MISSING" end
  if loaded[data.name] then return 1 end
  for _, dep in ipairs(data.deps) do
    if not loaded[known[strlower(dep)] or dep] then
      local ok = LoadAddOn(dep)
      if not ok then return nil, "DEP_MISSING" end
    end
  end
  loaded[data.name] = true
  if not __AndroidLoadToc(tocPath(data.name), data.name) then
    loaded[data.name] = nil
    return nil, "CORRUPT"
  end
  if __AndroidRestoreSaved then __AndroidRestoreSaved(data.fields) end
  __AndroidSignal("ADDON_LOADED", data.name)
  return 1
end
function IsAddOnLoaded(name)
  if type(name) == "number" then name = BLIZZARD[name] end
  local real = type(name) == "string" and (known[strlower(name)] or name)
  if real and loaded[real] then return 1 end
end
function IsAddOnLoadOnDemand(name)
  local data = type(name) == "string" and metadata(name)
  if data and data.loadOnDemand then return 1 end
end
-- GetNumAddOns lists user addons (Interface\AddOns on disk); none on Android yet.
function GetNumAddOns() return 0 end
function GetAddOnInfo(name)
  local data = type(name) == "string" and metadata(name)
  if not data then return end
  return data.name, data.fields["title"] or data.name, data.fields["notes"], 1, 1, nil, nil
end
function GetAddOnMetadata(name, field)
  local data = type(name) == "string" and metadata(name)
  return data and type(field) == "string" and data.fields[strlower(field)] or nil
end
function GetAddOnDependencies(name)
  local data = type(name) == "string" and metadata(name)
  if data then return unpack(data.deps) end
end
function EnableAddOn() end
function DisableAddOn() end
function EnableAllAddOns() end
function DisableAllAddOns() end
function IsAddonMessagePrefixRegistered() return nil end

-- Startup: every Blizzard addon that is not load-on-demand (AddOns.cpp at login).
function __AndroidLoadStartupAddOns()
  for _, name in ipairs(BLIZZARD) do
    local data = metadata(name)
    if data and not data.loadOnDemand then LoadAddOn(name) end
  end
end
