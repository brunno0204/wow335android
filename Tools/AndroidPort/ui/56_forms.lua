-- Android port: stances and shapeshift forms. The stance bar lists the known spells that apply
-- SPELL_AURA_MOD_SHAPESHIFT (36, misc value = SpellShapeshiftForm); the current form is
-- UNIT_FIELD_BYTES_2 byte 3 and its SpellShapeshiftForm.bonusActionBar is the bonus bar
-- (warrior stances, druid cat/bear/moonkin, rogue stealth...) that replaces action slots 1-12.
local Dbc = __AndroidDbc
local Field = __AndroidPlayerField
local signal = __AndroidSignal
local UNIT_FIELD_BYTES_2 = 0x7A

local forms = {}       -- index -> { spell, form }
local function rebuild()
  forms = {}
  local list = { __AndroidSpells() }
  local seen = {}
  for _, spell in ipairs(list) do
    local s = __AndroidSpellData(spell)
    if s and not s.passive and not seen[spell] then
      seen[spell] = true
      for i = 1, 3 do
        if Dbc("Spell", spell, "effectAura" .. i) == 36 then
          local form = Dbc("Spell", spell, "effectMiscValue" .. i) or 0
          if form > 0 then forms[#forms + 1] = { spell = spell, form = form } end
          break
        end
      end
    end
  end
  sort(forms, function(a, b) return a.form < b.form end)
end
local function currentForm() return bit.band(bit.rshift(Field(UNIT_FIELD_BYTES_2) or 0, 24), 0xff) end

function GetNumShapeshiftForms() return #forms end
function GetShapeshiftForm()
  local form = currentForm()
  for i, f in ipairs(forms) do if f.form == form then return i end end
  return 0
end
function GetShapeshiftFormID() local form = currentForm(); if form > 0 then return form end end
function GetShapeshiftFormInfo(index)
  local f = forms[tonumber(index) or 0]
  if not f then return end
  local s = __AndroidSpellData(f.spell)
  local start, duration = GetSpellCooldown(f.spell)
  return s and s.icon, s and s.name, currentForm() == f.form and 1 or nil, 1
end
function GetShapeshiftFormCooldown(index)
  local f = forms[tonumber(index) or 0]
  if f then return GetSpellCooldown(f.spell) end
  return 0, 0, 0
end
function CastShapeshiftForm(index)
  local f = forms[tonumber(index) or 0]
  if f then __AndroidCast(f.spell, "player") end
end
function GetBonusBarOffset()
  local form = currentForm()
  if form == 0 then return 0 end
  return Dbc("SpellShapeshiftForm", form, "bonusActionBar") or 0
end

-- Form and spell changes fire the original events (stance bar, bonus action bar).
local previousSpells = __AndroidSpellsChanged
function __AndroidSpellsChanged(...)
  if previousSpells then previousSpells(...) end
  rebuild()
  signal("UPDATE_SHAPESHIFT_FORMS")
end
local lastForm
local ticker = CreateFrame("Frame")
local acc = 0
ticker:SetScript("OnUpdate", function(self, elapsed)
  acc = acc + (elapsed or 0)
  if acc < .1 then return end
  acc = 0
  local form = currentForm()
  if form ~= lastForm then
    lastForm = form
    signal("UPDATE_SHAPESHIFT_FORM")
    signal("UPDATE_SHAPESHIFT_FORMS")
    signal("UPDATE_BONUS_ACTIONBAR")
    signal("ACTIONBAR_PAGE_CHANGED")
    signal("ACTIONBAR_UPDATE_STATE")
  end
end)
function __AndroidFormSpell(index) local f = forms[tonumber(index) or 0]; return f and f.spell end
