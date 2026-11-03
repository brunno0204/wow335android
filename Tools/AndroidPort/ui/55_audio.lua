-- Android port: zone music and ambience (AreaTable -> ZoneMusic / SoundAmbience -> SoundEntries,
-- day and night sets), interface and server sounds (PlaySound, PlaySoundFile, SMSG_PLAY_SOUND)
-- and the volume CVars, played by the Java audio player (__AndroidAudio command lines).
local Dbc = __AndroidDbc
local Audio = __AndroidAudio

-- SoundEntries: files (directoryBase + file1..10) and volume.
local entries = {}
local function entry(id)
  id = tonumber(id)
  if not id or id <= 0 then return end
  local e = entries[id]
  if e ~= nil then return e or nil end
  local name, base, volume = Dbc("SoundEntries", id, "name", "directoryBase", "volumeFloat")
  if not name then entries[id] = false; return end
  e = { name = name, volume = (volume and volume > 0) and volume or 1, files = {} }
  for i = 1, 10 do
    local file = Dbc("SoundEntries", id, "file" .. i)
    if file and file ~= "" then
      local path = (base and base ~= "") and (base .. "\\" .. file) or file
      e.files[#e.files + 1] = path
    end
  end
  entries[id] = e
  return e
end
local byName
local function entryByName(name)
  if not byName then
    byName = {}
    for i = 1, __AndroidDbcCount("SoundEntries") do
      local id, n = __AndroidDbcRow("SoundEntries", i, "id", "name")
      if id and n then byName[strlower(n)] = id end
    end
  end
  return byName[strlower(name or "")]
end

-- Volumes (the original sound options; Sound_EnableAllSound is the master switch).
local function cvarNumber(name, default)
  local v = tonumber(GetCVar(name))
  if v == nil then return default end
  return v
end
local function enabled(name) return GetCVar(name) ~= "0" end
function __AndroidAudioVolumes()
  Audio("volume", cvarNumber("Sound_MasterVolume", 1), enabled("Sound_EnableMusic") and cvarNumber("Sound_MusicVolume", .4) or 0,
        enabled("Sound_EnableSFX") and cvarNumber("Sound_SFXVolume", 1) or 0,
        enabled("Sound_EnableAmbience") and cvarNumber("Sound_AmbienceVolume", .6) or 0,
        enabled("Sound_EnableAllSound") and 1 or 0)
end

-- One-shot sounds.
local function playEntry(e, volume)
  if not e or #e.files == 0 then return end
  Audio("sound", (volume or 1) * e.volume, e.files[math.random(#e.files)])
end
function PlaySound(sound)
  local id = type(sound) == "number" and sound or tonumber(sound) or entryByName(sound)
  playEntry(entry(id))
end
function PlaySoundFile(path) if type(path) == "string" and path ~= "" then Audio("sound", 1, path) end end
function PlayMusic(path) if type(path) == "string" then Audio("music", 1, path, 0, 0) end end
function StopMusic() Audio("stop", "music") end
function Sound_GameSystem_RestartSoundSystem() __AndroidAudioVolumes() end
function Sound_GameSystem_GetNumOutputDrivers() return 1 end
function Sound_GameSystem_GetOutputDriverNameByIndex() return "Android" end
function Sound_GameSystem_GetNumInputDrivers() return 0 end
function VoiceChat_IsPlayingLoopbackSound() return nil end

-- Zone music and ambience: the area's set, falling back to its zone (parent area).
local currentMusic, currentAmbience
local function isNight()
  local hour = GetGameTime()
  return hour < 6 or hour >= 21
end
local function areaField(area, field)
  local value = Dbc("AreaTable", area, field)
  if (not value or value == 0) then
    local parent = Dbc("AreaTable", area, "parentAreaID")
    if parent and parent > 0 then value = Dbc("AreaTable", parent, field) end
  end
  return value
end
function __AndroidAudioArea(area)
  area = tonumber(area)
  if not area or area <= 0 then return end
  local night = isNight()
  local music = areaField(area, "zoneMusic")
  local key
  if music and music > 0 then
    local sound = Dbc("ZoneMusic", music, night and "sounds2" or "sounds1")
    if not sound or sound == 0 then sound = Dbc("ZoneMusic", music, "sounds1") end
    local e = entry(sound)
    if e and #e.files > 0 then
      key = music .. (night and "n" or "d")
      if key ~= currentMusic then
        currentMusic = key
        local silenceMin = Dbc("ZoneMusic", music, night and "silenceIntervalMin2" or "silenceIntervalMin1") or 0
        local silenceMax = Dbc("ZoneMusic", music, night and "silenceIntervalMax2" or "silenceIntervalMax1") or 0
        Audio("music", e.volume, table.concat(e.files, ";"), silenceMin, silenceMax)
      end
    end
  end
  if not key and currentMusic then currentMusic = nil; Audio("stop", "music") end
  local ambience = areaField(area, "ambienceID")
  local akey
  if ambience and ambience > 0 then
    local sound = Dbc("SoundAmbience", ambience, night and "ambienceID2" or "ambienceID1")
    local e = entry(sound)
    if e and #e.files > 0 then
      akey = ambience .. (night and "n" or "d")
      if akey ~= currentAmbience then
        currentAmbience = akey
        Audio("ambience", e.volume, e.files[1])
      end
    end
  end
  if not akey and currentAmbience then currentAmbience = nil; Audio("stop", "ambience") end
end

local H = __AndroidCombatHandlers
local lastArea
local previousArea = H.__ANDROID_AREA
H.__ANDROID_AREA = function(id, ...)
  if previousArea then previousArea(id, ...) end
  lastArea = tonumber(id)
  __AndroidAudioArea(lastArea)
end
local previousZone = H.__ANDROID_ZONE
H.__ANDROID_ZONE = function(map, zone, area, ...)
  if previousZone then previousZone(map, zone, area, ...) end
  if not lastArea then __AndroidAudioArea(tonumber(area) or tonumber(zone)) end
end
H.__ANDROID_PLAY_SOUND = function(id) PlaySound(tonumber(id)) end
-- Day/night change re-picks the sets; volume CVars follow the options panel.
local ticker = CreateFrame("Frame")
local acc, lastNight = 0, nil
ticker:SetScript("OnUpdate", function(self, elapsed)
  acc = acc + (elapsed or 0)
  if acc < 20 then return end
  acc = 0
  local night = isNight()
  if night ~= lastNight then lastNight = night; if lastArea then __AndroidAudioArea(lastArea) end end
end)
local setCVar = SetCVar
function SetCVar(name, value, ...)
  setCVar(name, value, ...)
  if type(name) == "string" and strfind(strlower(name), "^sound_") then __AndroidAudioVolumes() end
end
function __AndroidAudioLogin() __AndroidAudioVolumes() end

-- Spell sounds: SpellVisual precast kit when a cast starts, cast kit when it goes off
-- (SpellVisualKit.soundID), quieter for other units.
local function kitSound(spell, which, guid)
  local visual = Dbc("Spell", tonumber(spell) or 0, "spellVisualID")
  if not visual or visual <= 0 then return end
  local kit = Dbc("SpellVisual", visual, which)
  if not kit or kit <= 0 then return end
  local sound = Dbc("SpellVisualKit", kit, "soundID")
  local e = entry(sound)
  if e then playEntry(e, guid == __AndroidPlayerGuid() and 1 or .5) end
end
local castStart, castGo = H.__ANDROID_CAST_START, H.__ANDROID_CAST_GO
H.__ANDROID_CAST_START = function(guid, spell, ...)
  if castStart then castStart(guid, spell, ...) end
  kitSound(spell, "precastKit", guid)
end
H.__ANDROID_CAST_GO = function(guid, spell, ...)
  if castGo then castGo(guid, spell, ...) end
  kitSound(spell, "castKit", guid)
end
