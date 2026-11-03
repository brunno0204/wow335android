-- Android port: runs last, after every implemented API function is defined.
-- Error routing: print to the Android log instead of the original error dialog.
-- (00_core.lua keeps __AndroidErrorHandler installed.)

-- Every other original API name becomes a stub returning what the original returns when it
-- has no data ("Name:values" from tools/gen_ui_api.py: numbers 0, strings, or nothing).
local makers = {}
local function stub(values)
  if values == "" then return function() end end
  local maker = makers[values]
  if not maker then
    maker = loadstring("return function() return " .. values .. " end")
    makers[values] = maker
  end
  return maker and maker() or function() end
end
local function split(entry)
  local colon = strfind(entry, ":", 1, true)
  if not colon then return entry, "" end
  return strsub(entry, 1, colon - 1), strsub(entry, colon + 1)
end
for _, entry in ipairs(__AndroidApiNames) do
  local name, values = split(entry)
  if _G[name] == nil then
    if values == "" and strsub(name, 1, 6) == "GetNum" then values = "0" end
    _G[name] = stub(values)
  end
end

-- Game frame types without a native implementation are created as plain frames/models:
-- give them the original method names (tooltip, minimap, cooldown, model) as stubs.
do
  local function install(object)
    local methods = object and getmetatable(object) and getmetatable(object).__index
    if type(methods) ~= "table" then return end
    for _, entry in ipairs(__AndroidFrameMethods) do
      local name, values = split(entry)
      if methods[name] == nil then
        if name == "GetZoomLevels" then values = "1"
        elseif values == "" and (strsub(name, 1, 3) == "Num" or name == "GetZoom") then values = "0" end
        methods[name] = stub(values)
      end
    end
  end
  install(CreateFrame("Frame"))
  install(CreateFrame("Model"))
end
