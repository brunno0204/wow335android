-- Android port: PlayerModel/DressUpModel/TabardModel methods (CGCharacterModelBase) for the
-- model frames of UiModelFrames.cpp, which draw the unit's world model.
do
  local probe = CreateFrame("Model")
  local methods = probe and getmetatable(probe) and getmetatable(probe).__index
  if type(methods) == "table" then
    local units = setmetatable({}, { __mode = "k" })
    function methods:SetUnit(unit)
      units[self] = unit
      local guid = unit and UnitGUID(unit)
      __AndroidModelSetUnit(self, guid)
    end
    function methods:RefreshUnit() if units[self] then self:SetUnit(units[self]) end end
    function methods:ClearModel() units[self] = nil; __AndroidModelSetUnit(self, nil) end
    function methods:SetRotation(rotation) __AndroidModelSetRotation(self, tonumber(rotation) or 0) end
    function methods:GetRotation() return __AndroidModelGetRotation(self) end
    function methods:SetPortraitZoom(zoom) __AndroidModelSetZoom(self, 1 + (tonumber(zoom) or 0)) end
    function methods:SetCamDistanceScale(scale) __AndroidModelSetZoom(self, 1 / max(0.1, tonumber(scale) or 1)) end
    function methods:Dress() self:RefreshUnit() end
    function methods:Undress() end
    function methods:TryOn() end
    function methods:SetCreature() end
    function methods:SetDisplayInfo() end
    function methods:SetAnimation() end
  end
end
