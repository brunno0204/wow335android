-- Android port: login event sequence after FrameXML.toc loaded (CGGameUI::Initialize, then
-- entering the world). Called once by UiRuntime.
function __AndroidLoginEvents()
  local signal = __AndroidSignal
  if __AndroidSavedLoad then __AndroidSavedLoad() end
  if __AndroidLoadStartupAddOns then __AndroidLoadStartupAddOns() end
  if __AndroidMinimapSetup then __AndroidMinimapSetup() end
  if __AndroidInstallTooltips then __AndroidInstallTooltips() end
  signal("SPELLS_CHANGED")
  signal("VARIABLES_LOADED")
  if __AndroidChatLoginEvents then __AndroidChatLoginEvents() end
  signal("PLAYER_LOGIN")
  signal("PLAYER_ENTERING_WORLD")
  signal("UPDATE_BINDINGS")
  signal("ZONE_CHANGED_NEW_AREA")
  signal("PLAYER_TARGET_CHANGED")
  signal("ACTIONBAR_SLOT_CHANGED", 0)
  signal("UPDATE_SHAPESHIFT_FORMS")
  signal("BAG_UPDATE", 0)
  signal("PLAYER_MONEY")
  signal("UNIT_PORTRAIT_UPDATE", "player")
  if __AndroidDataLoginEvents then __AndroidDataLoginEvents() end
  if __AndroidSocialLogin then __AndroidSocialLogin() end
  if __AndroidApplyScale then __AndroidApplyScale() end
  if __AndroidRegisterSlash then __AndroidRegisterSlash() end
  if __AndroidAudioLogin then __AndroidAudioLogin() end
end
