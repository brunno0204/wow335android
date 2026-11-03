-- Android port: world map from WorldMapArea / WorldMapOverlay (zone art, all overlays shown as
-- explored), continents and zones in the original order, the player's position on the map.
local Dbc = __AndroidDbc
local signal = __AndroidSignal
-- Continent index (the original's order) -> map ID.
local CONTINENT_MAPS = { 1, 0, 530, 571 }
local rows, byArea, continentRow, zonesOf
local function load()
  if rows then return end
  rows, byArea, continentRow, zonesOf = {}, {}, {}, {}
  for i = 1, __AndroidDbcCount("WorldMapArea") do
    local id, map, areaId, file, left, right, top, bottom =
      __AndroidDbcRow("WorldMapArea", i, "id", "mapID", "areaID", "areaName", "locLeft", "locRight", "locTop", "locBottom")
    if id then
      local r = { id = id, map = map, area = areaId, file = file, left = left, right = right, top = top, bottom = bottom }
      rows[id] = r
      if areaId and areaId > 0 then byArea[areaId] = byArea[areaId] or r end
      for c, m in ipairs(CONTINENT_MAPS) do
        if map == m then
          if areaId == 0 then continentRow[c] = continentRow[c] or r
          else
            zonesOf[c] = zonesOf[c] or {}
            r.name = __AndroidAreaName and __AndroidAreaName(areaId) or file
            tinsert(zonesOf[c], r)
          end
        end
      end
    end
  end
  for _, list in pairs(zonesOf) do sort(list, function(a, b) return (a.name or "") < (b.name or "") end) end
end

local state = { continent = 0, zone = 0, row = nil }
local function setState(continent, zone)
  load()
  state.continent, state.zone = continent or 0, zone or 0
  if state.continent > 0 then
    state.row = state.zone > 0 and zonesOf[state.continent] and zonesOf[state.continent][state.zone]
                or continentRow[state.continent]
  else
    state.row = nil
  end
  signal("WORLD_MAP_UPDATE")
end
function SetMapToCurrentZone()
  load()
  local zone, area = 0, 0
  if __AndroidCurrentZone then zone, area = __AndroidCurrentZone() end
  local map = __AndroidPlayerPose()
  local target = byArea[zone] or byArea[area]
  for c, m in ipairs(CONTINENT_MAPS) do
    if target and target.map == m or (not target and map == m) then
      if target then
        for i, r in ipairs(zonesOf[c] or {}) do
          if r == target then setState(c, i); return end
        end
      end
      setState(c, 0)
      return
    end
  end
  setState(0, 0)
end
function SetMapZoom(continent, zone) setState(tonumber(continent) or 0, tonumber(zone) or 0) end
function ZoomOut()
  if state.zone > 0 then setState(state.continent, 0)
  elseif state.continent > 0 then setState(0, 0)
  else setState(-1, 0) end
end
function IsZoomOutAvailable() if state.continent ~= -1 then return 1 end end
function GetCurrentMapContinent() return state.continent end
function GetCurrentMapZone() return state.zone end
function GetCurrentMapAreaID() return state.row and state.row.id or 0 end
function GetCurrentMapDungeonLevel() return 0 end
function GetNumDungeonMapLevels() return 0 end
function DungeonUsesTerrainMap() return nil end
function SetDungeonMapLevel() end
function GetMapInfo()
  if state.row and state.row.file and state.row.file ~= "" then return state.row.file, 768, 1024 end
end
function GetMapContinents()
  load()
  local out = {}
  for c = 1, #CONTINENT_MAPS do
    local r = continentRow[c]
    out[#out + 1] = r and (Dbc("Map", r.map, "mapName") or r.file) or ""
  end
  return unpack(out)
end
function GetMapZones(continent)
  load()
  local out = {}
  for _, r in ipairs(zonesOf[tonumber(continent) or 0] or {}) do out[#out + 1] = r.name end
  return unpack(out)
end
-- Overlays: every WorldMapOverlay of the map area, shown as explored.
local overlayCache = {}
local function overlays()
  local r = state.row
  if not r then return {} end
  local list = overlayCache[r.id]
  if list then return list end
  list = {}
  for i = 1, __AndroidDbcCount("WorldMapOverlay") do
    local mapArea, name, w, h, ox, oy, px, py = __AndroidDbcRow("WorldMapOverlay", i, "mapAreaID", "textureName",
      "textureWidth", "textureHeight", "offsetX", "offsetY", "mapPointX", "mapPointY")
    if mapArea == r.id and name and name ~= "" then
      list[#list + 1] = { "Interface\\WorldMap\\" .. r.file .. "\\" .. name, w or 0, h or 0, ox or 0, oy or 0, px or 0, py or 0 }
    end
  end
  overlayCache[r.id] = list
  return list
end
function GetNumMapOverlays() return #overlays() end
function GetMapOverlayInfo(i) local o = overlays()[tonumber(i) or 0]; if o then return unpack(o) end end
function GetNumMapLandmarks() return 0 end
function GetMapLandmarkInfo() return nil end
-- Position on the current map (0..1), 0, 0 when the unit is not on it.
function GetPlayerMapPosition(unit)
  local r = state.row
  if not r or (unit ~= "player" and not UnitIsUnit(unit or "", "player")) then return 0, 0 end
  local map, x, y = __AndroidPlayerPose()
  if not map or map ~= r.map or not r.left or r.left == r.right or r.top == r.bottom then return 0, 0 end
  local px = (r.left - y) / (r.left - r.right)
  local py = (r.top - x) / (r.top - r.bottom)
  if px < 0 or px > 1 or py < 0 or py > 1 then return 0, 0 end
  return px, py
end
