-- Android port: Minimap frame content (CGMinimapFrame is not compiled in): the composed tile
-- texture (UiMinimap.cpp), the player arrow and zoom levels.
local zoom = 0
function GetPlayerFacing()
  local _, _, _, _, heading = __AndroidPlayerPose()
  return heading
end
function GetPlayerMapPosition() return 0, 0 end

function __AndroidMinimapSetup()
  local minimap = Minimap
  if not minimap or minimap.androidImage then return end
  local image = minimap:CreateTexture("MinimapAndroidImage", "BACKGROUND")
  image:SetAllPoints(minimap)
  __AndroidMinimapAttach(image)
  minimap.androidImage = image
  local arrow = minimap:CreateTexture("MinimapAndroidArrow", "OVERLAY")
  arrow:SetTexture("Interface\\Minimap\\MinimapArrow")
  arrow:SetWidth(32)
  arrow:SetHeight(32)
  arrow:SetPoint("CENTER", minimap, "CENTER", 0, 0)
  minimap.androidArrow = arrow
  minimap.GetZoom = function() return zoom end
  minimap.GetZoomLevels = function() return 6 end
  minimap.SetZoom = function(self, level)
    zoom = max(0, min(5, tonumber(level) or 0))
    __AndroidMinimapZoom(zoom)
    __AndroidSignal("MINIMAP_UPDATE_ZOOM")
  end
  minimap.PingLocation = function() end
  minimap.GetPingPosition = function() return 0, 0 end
  local updater = CreateFrame("Frame", nil, minimap)
  local elapsedTotal = 0
  updater:SetScript("OnUpdate", function(self, elapsed)
    elapsedTotal = elapsedTotal + elapsed
    if elapsedTotal < 0.05 then return end
    elapsedTotal = 0
    local heading = GetPlayerFacing()
    if heading and arrow.SetRotation then arrow:SetRotation(heading) end
  end)
end

-- Battleground positions on the world map (no battlegrounds yet): the map does arithmetic on them.
function GetNumBattlefieldPositions() return 0 end
function GetBattlefieldPosition() return 0, 0, nil end
function GetNumBattlefieldFlagPositions() return 0 end
function GetBattlefieldFlagPosition() return 0, 0, nil end
function GetNumBattlefieldVehicles() return 0 end
function GetBattlefieldVehicleInfo() return 0, 0, nil end
function GetCorpseMapPosition() return 0, 0 end
function GetDeathReleasePosition() return 0, 0 end
