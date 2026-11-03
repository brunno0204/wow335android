-- Android port: one-shot UI diagnostics in the status log (unit frame bars), 20 s into the world.
local diag = CreateFrame("Frame")
local waited = 0
diag:SetScript("OnUpdate", function(self, elapsed)
  waited = waited + (elapsed or 0)
  if waited < 20 then return end
  self:SetScript("OnUpdate", nil)
  local function bar(name)
    local b = _G[name]
    if not b then return name .. "=nil" end
    local mn, mx = b:GetMinMaxValues()
    local t = b.GetStatusBarTexture and b:GetStatusBarTexture()
    local r, g, bl = 0, 0, 0
    if b.GetStatusBarColor then r, g, bl = b:GetStatusBarColor() end
    return format("%s %s/%s(%s) vis=%s a=%.2f tex=%s w=%.0f cor=%.1f,%.1f,%.1f", name, tostring(b:GetValue()), tostring(mx),
      tostring(mn), tostring(b:IsVisible()), b:GetEffectiveAlpha() or -1, tostring(t and t:GetTexture()),
      (t and t:GetWidth()) or -1, r or 0, g or 0, bl or 0)
  end
  __AndroidLog("diag: " .. bar("PlayerFrameHealthBar"))
  __AndroidLog("diag: " .. bar("PlayerFrameManaBar"))
  __AndroidLog(format("diag: player hp %s/%s power %s/%s", tostring(UnitHealth("player")), tostring(UnitHealthMax("player")),
    tostring(UnitPower("player")), tostring(UnitPowerMax("player"))))
end)
