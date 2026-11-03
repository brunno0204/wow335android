-- Android port: saved variables (WTF/Account.lua and WTF/<realm>-<character>.lua in the app's
-- files), macros, the CVars the player changes and the interface scale for phone screens.
local signal = __AndroidSignal

-- ---- Serializer: plain data (strings, numbers, booleans, nested tables without cycles).
local function serialize(value, out, indent, seen)
  local kind = type(value)
  if kind == "string" then out[#out + 1] = format("%q", value)
  elseif kind == "number" then
    if value ~= value or value == math.huge or value == -math.huge then out[#out + 1] = "0"
    else out[#out + 1] = format("%.17g", value) end
  elseif kind == "boolean" then out[#out + 1] = value and "true" or "false"
  elseif kind == "table" and not seen[value] then
    seen[value] = true
    out[#out + 1] = "{\n"
    for k, v in pairs(value) do
      local kk, vk = type(k), type(v)
      if (kk == "string" or kk == "number" or kk == "boolean") and
         (vk == "string" or vk == "number" or vk == "boolean" or vk == "table") then
        out[#out + 1] = indent .. "  ["
        serialize(k, out, "", seen)
        out[#out + 1] = "] = "
        serialize(v, out, indent .. "  ", seen)
        out[#out + 1] = ",\n"
      end
    end
    out[#out + 1] = indent .. "}"
    seen[value] = nil
  else out[#out + 1] = "nil" end
end
local function parse(text)
  local env = {}
  if not text then return env end
  local chunk = loadstring(text)
  if not chunk then return env end
  setfenv(chunk, env)
  pcall(chunk)
  return env
end

-- ---- Stores: account and character.
local account, character = {}, {}
local accountNames, characterNames = {}, {}  -- addon saved variable names
local loadedStores = false
local function characterFile()
  return gsub((GetRealmName() or "realm") .. "-" .. (UnitName("player") or "char"), "[^%w%-_]", "_") .. ".lua"
end
function __AndroidSavedLoad()
  if loadedStores then return end
  loadedStores = true
  account = parse(__AndroidLoadFile("Account.lua"))
  character = parse(__AndroidLoadFile(characterFile()))
  -- Port state lives in the stores themselves.
  account.ANDROID_CVARS = type(account.ANDROID_CVARS) == "table" and account.ANDROID_CVARS or {}
  account.ANDROID_MACROS = type(account.ANDROID_MACROS) == "table" and account.ANDROID_MACROS or {}
  character.ANDROID_MACROS = type(character.ANDROID_MACROS) == "table" and character.ANDROID_MACROS or {}
  character.ANDROID_CVARS = type(character.ANDROID_CVARS) == "table" and character.ANDROID_CVARS or {}
  for name, value in pairs(account.ANDROID_CVARS) do __AndroidRawSetCVar(name, value) end
  for name, value in pairs(character.ANDROID_CVARS) do __AndroidRawSetCVar(name, value) end
end
-- Called by LoadAddOn after the addon's files ran, before ADDON_LOADED.
function __AndroidRestoreSaved(fields)
  local function names(list)
    local t = {}
    if list then for name in gmatch(list, "[^,%s]+") do t[#t + 1] = name end end
    return t
  end
  for _, name in ipairs(names(fields and fields["savedvariables"])) do
    accountNames[#accountNames + 1] = name
    if account[name] ~= nil then _G[name] = account[name] end
  end
  for _, name in ipairs(names(fields and fields["savedvariablespercharacter"])) do
    characterNames[#characterNames + 1] = name
    if character[name] ~= nil then _G[name] = character[name] end
  end
end
local lastAccount, lastCharacter
function __AndroidSavedWrite()
  if not loadedStores then return end
  for _, name in ipairs(accountNames) do account[name] = _G[name] end
  for _, name in ipairs(characterNames) do character[name] = _G[name] end
  local function storeText(store)
    local out = {}
    local keys = {}
    for k in pairs(store) do if type(k) == "string" then keys[#keys + 1] = k end end
    sort(keys)
    for _, k in ipairs(keys) do
      out[#out + 1] = k .. " = "
      serialize(store[k], out, "", {})
      out[#out + 1] = "\n"
    end
    return table.concat(out)
  end
  local a = storeText(account)
  local c = storeText(character)
  if a ~= lastAccount and __AndroidSaveFile("Account.lua", a) then lastAccount = a end
  if c ~= lastCharacter and __AndroidSaveFile(characterFile(), c) then lastCharacter = c end
end

-- ---- CVars the player changes are kept (SetCVar is defined in 00_core.lua).
local coreSetCVar = SetCVar
__AndroidRawSetCVar = coreSetCVar
function SetCVar(name, value, event)
  coreSetCVar(name, value)
  if type(name) ~= "string" then return end
  if loadedStores then
    local key, text = strlower(name), tostring(value or "")
    if text == GetCVarDefault(name) then account.ANDROID_CVARS[key] = nil else account.ANDROID_CVARS[key] = text end
  end
  if event then signal("CVAR_UPDATE", event, tostring(value or "")) end
  if strlower(name) == "uiscale" or strlower(name) == "useuiscale" then __AndroidApplyScale() end
end

-- ---- Interface scale: phones get a larger UI (CVar uiScale, applied to UIParent).
local function defaultScale()
  local w, h = __AndroidPixels()
  if not w or not h or h <= 0 then return 1 end
  -- 768 UI units over the screen height: on a short landscape phone screen everything is tiny.
  if w / h > 1.8 then return 1.2 end
  return 1
end
function __AndroidApplyScale()
  local scale = tonumber(GetCVar("uiScale")) or 1
  if GetCVar("useUiScale") ~= "1" then scale = defaultScale() end
  scale = math.min(1.6, math.max(0.6, scale))
  if UIParent and UIParent.SetScale then UIParent:SetScale(scale) end
  signal("UI_SCALE_CHANGED")
end
local function scaleCommand(text)
  local value = tonumber(text)
  if value then
    SetCVar("useUiScale", "1")
    SetCVar("uiScale", tostring(value))
  else
    SetCVar("useUiScale", "0")
    __AndroidApplyScale()
  end
  DEFAULT_CHAT_FRAME:AddMessage(format("Escala da interface: %.2f (/escala 0.8–1.6; /escala sem número = automática)",
                                       UIParent:GetScale()), 1, 1, 0)
end
-- ChatFrame.lua builds SlashCmdList and its hash at load; ours are added at login.
function __AndroidRegisterSlash()
  SLASH_ANDROIDSCALE1 = "/escala"
  SLASH_ANDROIDSCALE2 = "/uiscale"
  SlashCmdList["ANDROIDSCALE"] = scaleCommand
  if hash_SlashCmdList then
    hash_SlashCmdList["/ESCALA"] = scaleCommand
    hash_SlashCmdList["/UISCALE"] = scaleCommand
  end
end

-- ---- Macros: 36 account (1-36) and 18 character (37-54) macros, as in the original.
local MAX_ACCOUNT, MAX_CHARACTER = 36, 18
local function lists() return account.ANDROID_MACROS or {}, character.ANDROID_MACROS or {} end
local function macro(index)
  index = tonumber(index)
  if not index then return end
  local a, c = lists()
  if index >= 1 and index <= MAX_ACCOUNT then return a[index], a, index end
  if index > MAX_ACCOUNT and index <= MAX_ACCOUNT + MAX_CHARACTER then return c[index - MAX_ACCOUNT], c, index - MAX_ACCOUNT end
end
local function iconPath(icon)
  if type(icon) == "number" then return GetMacroIconInfo(icon) end
  if type(icon) == "string" and icon ~= "" then
    if not strfind(icon, "\\") then return "Interface\\Icons\\" .. icon end
    return icon
  end
  return "Interface\\Icons\\INV_Misc_QuestionMark"
end
function GetNumMacros() local a, c = lists(); return #a, #c end
function GetMacroInfo(index)
  if type(index) == "string" then index = GetMacroIndexByName(index) end
  local m = macro(index)
  if m then return m.name, iconPath(m.icon), m.body, (tonumber(index) or 0) > MAX_ACCOUNT and 1 or nil end
end
function GetMacroIndexByName(name)
  local a, c = lists()
  for i, m in ipairs(a) do if m.name == name then return i end end
  for i, m in ipairs(c) do if m.name == name then return MAX_ACCOUNT + i end end
  return 0
end
function GetMacroBody(index) local m = macro(type(index) == "string" and GetMacroIndexByName(index) or index); return m and m.body end
local function macrosChanged() signal("UPDATE_MACROS"); signal("ACTIONBAR_SLOT_CHANGED", 0); __AndroidSavedWrite() end
function CreateMacro(name, icon, body, perCharacter)
  local a, c = lists()
  local list, offset, max = a, 0, MAX_ACCOUNT
  if perCharacter and perCharacter ~= 0 then list, offset, max = c, MAX_ACCOUNT, MAX_CHARACTER end
  if #list >= max then return end
  list[#list + 1] = { name = name or "", icon = icon, body = body or "" }
  sort(list, function(x, y) return strlower(x.name) < strlower(y.name) end)
  macrosChanged()
  for i, m in ipairs(list) do if m.name == name then return offset + i end end
end
function EditMacro(index, name, icon, body)
  local m = macro(index)
  if not m then return end
  if name then m.name = name end
  if icon then m.icon = icon end
  if body then m.body = body end
  macrosChanged()
  return index
end
function DeleteMacro(index)
  if type(index) == "string" then index = GetMacroIndexByName(index) end
  local m, list, i = macro(index)
  if m then tremove(list, i); macrosChanged() end
end
local icons
local function iconList()
  if icons then return icons end
  icons = { "Interface\\Icons\\INV_Misc_QuestionMark" }
  local seen = {}
  for i = 1, __AndroidDbcCount("SpellIcon") do
    local path = __AndroidDbcRow("SpellIcon", i, "textureFilename")
    if path and path ~= "" and not seen[strlower(path)] and strfind(strlower(path), "interface\\icons\\", 1, true) then
      seen[strlower(path)] = true
      icons[#icons + 1] = path
    end
  end
  return icons
end
function GetNumMacroIcons() return #iconList() end
function GetNumMacroItemIcons() return 0 end
function GetMacroIconInfo(i) return iconList()[tonumber(i) or 0] end
function GetMacroItemIconInfo() return nil end
function GetMacroSpell(index)
  local m = macro(index)
  local spell = m and strmatch(m.body, "/cast%s+([^\n;]+)")
  if spell then spell = gsub(gsub(spell, "%[.-%]", ""), "^%s+", ""); return spell, "" end
end
function GetMacroItem() return nil end
function PickupMacro(index)
  local m = macro(type(index) == "string" and GetMacroIndexByName(index) or index)
  if m then __AndroidSetCursor({ kind = "action", actionType = "macro", id = index, value = bit.bor(index, bit.lshift(64, 24)) }) end
end
-- Runs the macro lines through the chat edit box like typing them (slash commands, /cast...).
function RunMacroText(text)
  local box = ChatFrameEditBox or ChatFrame1EditBox or DEFAULT_CHAT_FRAME.editBox
  if not box or not text then return end
  for line in gmatch(text, "[^\r\n]+") do
    if strsub(line, 1, 1) == "/" then
      box:SetText(line)
      ChatEdit_SendText(box)
    end
  end
end
function RunMacro(index)
  local m = macro(type(index) == "string" and GetMacroIndexByName(index) or index)
  if m then RunMacroText(m.body) end
end
-- Action buttons holding macros (action type 64).
local getActionTexture, getActionText, useAction = GetActionTexture, GetActionText, UseAction
function GetActionTexture(slot)
  local id, kind = __AndroidAction(tonumber(slot) or 0)
  if id and kind == 64 then local _, icon = GetMacroInfo(id); return icon end
  return getActionTexture(slot)
end
function GetActionText(slot)
  local id, kind = __AndroidAction(tonumber(slot) or 0)
  if id and kind == 64 then return (GetMacroInfo(id)) end
  return getActionText(slot)
end
function UseAction(slot, unit, ...)
  local id, kind = __AndroidAction(tonumber(slot) or 0)
  if id and kind == 64 then RunMacro(id); return end
  return useAction(slot, unit, ...)
end

-- Periodic save (Android may stop the app at any time) and on logout.
local saver = CreateFrame("Frame")
local acc = 0
saver:SetScript("OnUpdate", function(self, elapsed)
  acc = acc + (elapsed or 0)
  if acc >= 30 then acc = 0; __AndroidSavedWrite() end
end)
saver:RegisterEvent("PLAYER_LOGOUT")
saver:RegisterEvent("PLAYER_LEAVING_WORLD")
saver:SetScript("OnEvent", function() __AndroidSavedWrite() end)

-- ---- Macro conditionals ([target=x], [@x], combat, harm, help, exists, dead, mod...) like the
-- client's SecureCmdOptionParse: the first clause whose conditions hold gives the action and unit.
local function condition(test, target)
  local negate = false
  local name, arg = strmatch(test, "^%s*([%w@]+)%s*[:=]?%s*(.-)%s*$")
  if not name then return true end
  name = strlower(name)
  if strsub(name, 1, 2) == "no" and name ~= "nodead" or name == "nodead" then negate = true; name = strsub(name, 3) end
  local result
  if name == "combat" then result = UnitAffectingCombat("player") ~= nil
  elseif name == "exists" then result = UnitExists(target) ~= nil
  elseif name == "dead" then result = UnitIsDead(target) ~= nil
  elseif name == "harm" then result = UnitCanAttack("player", target) ~= nil
  elseif name == "help" then result = UnitCanAttack("player", target) == nil and UnitExists(target) ~= nil
  elseif name == "mod" or name == "modifier" then result = IsModifierKeyDown and IsModifierKeyDown() ~= nil or false
  elseif name == "stance" or name == "form" then
    local form = GetShapeshiftForm and GetShapeshiftForm() or 0
    if arg == "" then result = form > 0 else result = false; for n in gmatch(arg, "%d+") do if tonumber(n) == form then result = true end end end
  elseif name == "party" then result = UnitInParty(target) ~= nil
  elseif name == "raid" then result = UnitInRaid(target) ~= nil
  elseif name == "mounted" then result = IsMounted and IsMounted() ~= nil or false
  elseif name == "swimming" then result = IsSwimming and IsSwimming() ~= nil or false
  elseif name == "indoors" or name == "outdoors" or name == "flyable" or name == "flying" or name == "stealth" then result = name == "outdoors"
  else result = true end
  if negate then return not result end
  return result
end
function SecureCmdOptionParse(text)
  if type(text) ~= "string" then return end
  for clause in gmatch(text .. ";", "([^;]*);") do
    local target = "target"
    local ok = true
    local rest = clause
    local sawConditions = false
    while true do
      local block, after = strmatch(rest, "^%s*%[(.-)%](.*)$")
      if not block then break end
      sawConditions = true
      rest = after
      local blockTarget = "target"
      local blockOk = true
      for test in gmatch(block, "[^,]+") do
        local unit = strmatch(test, "^%s*target%s*=%s*(.-)%s*$") or strmatch(test, "^%s*@%s*(.-)%s*$")
        if unit then blockTarget = unit
        elseif not condition(test, blockTarget) then blockOk = false end
      end
      target = blockTarget
      if blockOk then ok = true; break else ok = false end
    end
    if ok then
      local action = strmatch(rest, "^%s*(.-)%s*$")
      if not sawConditions then return action, nil end
      return action, target
    end
  end
end
function SecureCmdItemParse(text)
  if not text then return end
  local bag, slot = strmatch(text, "^(%d+)%s+(%d+)$")
  if bag then return nil, tonumber(bag), tonumber(slot) end
  slot = tonumber(text)
  if slot then return nil, nil, slot end
  return text
end
function SecureCmdUseItem(name, bag, slot, target)
  if bag then UseContainerItem(bag, slot) elseif slot then UseInventoryItem(slot) elseif name then UseItemByName(name) end
end

-- Game menu Logout / Exit Game: the app leaves the world (like the old "Sair do mundo").
function Logout() __AndroidSavedWrite(); __AndroidAudio("app", "logout") end
function Quit() __AndroidSavedWrite(); __AndroidAudio("app", "logout") end
function ForceQuit() Quit() end
function ForceLogout() Logout() end
function CancelLogout() end
function IsCurrentlyLoggingOut() return nil end

-- Android back button = Escape. A full-screen panel (world map) hides UIParent; close it first.
function __AndroidEscape()
  if WorldMapFrame and WorldMapFrame:IsShown() then HideUIPanel(WorldMapFrame); return end
  if not UIParent:IsShown() then UIParent:Show() end
  ToggleGameMenu()
end
