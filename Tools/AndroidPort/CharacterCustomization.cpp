#include "CharacterCustomization.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
namespace wow::android {
namespace {
constexpr int SKIN = 0, FACE = 1, FACIAL = 2, HAIR = 3, UNDERWEAR = 4;
// CCharacterComponent::s_sectionInfoRaw (512x512 base).
constexpr uint32_t sectionRaw[10][4] = {{0, 0, 256, 128},   {0, 128, 256, 128}, {0, 256, 256, 64},
                                        {256, 0, 256, 128}, {256, 128, 256, 64}, {256, 192, 256, 128},
                                        {256, 320, 256, 128}, {256, 448, 256, 64}, {0, 320, 256, 64},
                                        {0, 384, 256, 128}};
uint32_t key(uint32_t race, uint32_t sex) { return race * 2 + sex; }
std::string lower(std::string s) {
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace
std::array<uint32_t, 4> characterSectionRect(CharacterSection s, uint32_t w, uint32_t h) {
    const auto &r = sectionRaw[s];
    return {r[0] * w / 512, r[1] * h / 512, r[2] * w / 512, r[3] * h / 512};
}
const std::array<const char *, 11> &characterDbcPaths() {
    static const std::array<const char *, 11> paths{
        "DBFilesClient\\ChrRaces.dbc",          "DBFilesClient\\ChrClasses.dbc",
        "DBFilesClient\\CharBaseInfo.dbc",      "DBFilesClient\\CharSections.dbc",
        "DBFilesClient\\CharHairGeosets.dbc",   "DBFilesClient\\CharacterFacialHairStyles.dbc",
        "DBFilesClient\\CreatureDisplayInfo.dbc", "DBFilesClient\\CreatureModelData.dbc",
        "DBFilesClient\\FactionTemplate.dbc",   "DBFilesClient\\FactionGroup.dbc",
        "DBFilesClient\\NameGen.dbc"};
    return paths;
}
CharacterCustomization::CharacterCustomization(const CharacterDbcFiles &f) {
    const DbcTable races(f.chrRaces, 69, 276, "ChrRaces.dbc"),
        classes(f.chrClasses, 60, 240, "ChrClasses.dbc"),
        base(f.charBaseInfo, 2, 2, "CharBaseInfo.dbc"),
        sections(f.charSections, 10, 40, "CharSections.dbc"),
        hair(f.charHairGeosets, 6, 24, "CharHairGeosets.dbc"),
        facial(f.facialHairStyles, 8, 32, "CharacterFacialHairStyles.dbc"),
        display(f.creatureDisplayInfo, 16, 64, "CreatureDisplayInfo.dbc"),
        model(f.creatureModelData, 28, 112, "CreatureModelData.dbc"),
        templates(f.factionTemplate, 14, 56, "FactionTemplate.dbc"),
        groups(f.factionGroup, 20, 80, "FactionGroup.dbc"), names(f.nameGen, 4, 16, "NameGen.dbc");
    for (uint32_t r = 0; r < classes.rows(); ++r) {
        ClassInfo c;
        c.id = classes.u32(r, 0);
        c.name = classes.loc(r, 4);
        c.nameFemale = classes.loc(r, 21);
        c.nameMale = classes.loc(r, 38);
        c.filename = classes.str(r, 55);
        c.requiredExpansion = classes.u32(r, 59);
        classes_.push_back(std::move(c));
    }
    for (uint32_t r = 0; r < base.rows(); ++r)
        baseInfo_.push_back({base.byte(r, 0), base.byte(r, 1)});
    std::map<uint32_t, uint32_t> templateGroup;
    for (uint32_t r = 0; r < templates.rows(); ++r)
        templateGroup[templates.u32(r, 0)] = templates.u32(r, 3);
    for (uint32_t r = 0; r < races.rows(); ++r) {
        RaceInfo info;
        info.id = races.u32(r, 0);
        info.flags = races.u32(r, 1);
        info.maleDisplay = races.u32(r, 4);
        info.femaleDisplay = races.u32(r, 5);
        info.clientPrefix = races.str(r, 6);
        info.fileString = races.str(r, 11);
        info.name = races.loc(r, 14);
        info.nameFemale = races.loc(r, 31);
        info.nameMale = races.loc(r, 48);
        info.facialToken = {races.str(r, 65), races.str(r, 66)};
        info.hairToken = races.str(r, 67);
        info.requiredExpansion = races.u32(r, 68);
        info.faction = -1;
        const auto group = templateGroup.find(races.u32(r, 2));
        if (group != templateGroup.end())
            for (uint32_t g = 0; g < groups.rows(); ++g) {
                const uint32_t mask = groups.u32(g, 1);
                if (!mask || mask > 31 || !((1u << mask) & group->second))
                    continue;
                const auto internal = lower(groups.str(g, 2));
                if (internal == "alliance")
                    info.faction = 0;
                else if (internal == "horde")
                    info.faction = 1;
            }
        allRaces_.push_back(info);
    }
    for (int faction = 0; faction < 2; ++faction)
        for (const auto &r : allRaces_)
            if (!(r.flags & 1) && r.faction == faction)
                races_.push_back(r);
    sections_.reserve(sections.rows());
    for (uint32_t r = 0; r < sections.rows(); ++r) {
        Section s;
        s.race = sections.i32(r, 1);
        s.sex = sections.i32(r, 2);
        s.base = sections.i32(r, 3);
        s.texture = {sections.str(r, 4), sections.str(r, 5), sections.str(r, 6)};
        s.flags = sections.i32(r, 7);
        s.variation = sections.i32(r, 8);
        s.color = sections.i32(r, 9);
        if (s.base < 0 || s.base >= 5 || s.race < 0 || s.race > 255 || s.sex < 0 || s.sex > 1 ||
            s.variation < 0 || s.variation > 255 || s.color < 0 || s.color > 255)
            continue;
        sections_.push_back(std::move(s));
    }
    // BuildComponentArray: variation/color counts are max index + 1 per group.
    for (const auto &s : sections_) {
        auto &list = lookup_[key(s.race, s.sex)][s.base];
        if (list.size() <= size_t(s.variation))
            list.resize(s.variation + 1);
        auto &colors = list[s.variation];
        if (colors.size() <= size_t(s.color))
            colors.resize(s.color + 1, nullptr);
    }
    for (const auto &s : sections_)
        lookup_[key(s.race, s.sex)][s.base][s.variation][s.color] = &s;
    for (uint32_t r = 0; r < hair.rows(); ++r)
        hairGeosets_.push_back({hair.i32(r, 1), hair.i32(r, 2), hair.i32(r, 3), hair.i32(r, 4),
                                hair.i32(r, 5)});
    for (uint32_t r = 0; r < facial.rows(); ++r) {
        FacialStyle s{facial.i32(r, 0), facial.i32(r, 1), facial.i32(r, 2), {}};
        for (int i = 0; i < 5; ++i)
            s.geoset[i] = facial.i32(r, 3 + i);
        facialStyles_.push_back(s);
        if (s.race >= 0 && s.sex >= 0)
            ++facialCounts_[key(s.race, s.sex)];
    }
    for (uint32_t r = 0; r < display.rows(); ++r)
        displayModel_[display.u32(r, 0)] = display.u32(r, 1);
    for (uint32_t r = 0; r < model.rows(); ++r)
        modelNames_[model.u32(r, 0)] = model.str(r, 2);
    for (uint32_t r = 0; r < names.rows(); ++r)
        names_.push_back({names.str(r, 1), names.i32(r, 2), names.i32(r, 3)});
    if (races_.empty())
        throw std::invalid_argument("No playable races in ChrRaces.dbc");
}
const RaceInfo *CharacterCustomization::race(uint32_t id) const {
    for (const auto &r : allRaces_)
        if (r.id == id)
            return &r;
    return nullptr;
}
const ClassInfo *CharacterCustomization::classInfo(uint32_t id) const {
    for (const auto &c : classes_)
        if (c.id == id)
            return &c;
    return nullptr;
}
std::vector<ClassInfo> CharacterCustomization::classesForRace(uint32_t race) const {
    std::vector<ClassInfo> out;
    for (const auto &[r, c] : baseInfo_)
        if (r == race)
            if (const auto *info = classInfo(c))
                out.push_back(*info);
    return out;
}
bool CharacterCustomization::raceClassValid(uint32_t race, uint32_t cls) const {
    for (const auto &[r, c] : baseInfo_)
        if (r == race && c == cls)
            return true;
    return false;
}
const CharacterCustomization::Section *CharacterCustomization::get(uint32_t race, uint32_t sex,
                                                                    int base, int variation,
                                                                    int color) const {
    const auto found = lookup_.find(key(race, sex));
    if (found == lookup_.end() || variation < 0 || color < 0)
        return nullptr;
    const auto &list = found->second[base];
    if (size_t(variation) >= list.size() || size_t(color) >= list[variation].size())
        return nullptr;
    return list[variation][color];
}
int CharacterCustomization::numVariations(uint32_t race, uint32_t sex, int base) const {
    const auto found = lookup_.find(key(race, sex));
    return found == lookup_.end() ? 0 : int(found->second[base].size());
}
int CharacterCustomization::numColors(uint32_t race, uint32_t sex, int base, int variation) const {
    const auto found = lookup_.find(key(race, sex));
    if (found == lookup_.end() || variation < 0 || size_t(variation) >= found->second[base].size())
        return 0;
    return int(found->second[base][variation].size());
}
// ComponentFlagsMatch with GetSelectionFromContext(COMPONENT_CONTEXT_0, class).
bool CharacterCustomization::matches(int32_t f, uint8_t cls) const {
    if (cls == 6)
        return (f & 0x1) && (f & 0x14) && !(f & 0x8);
    return (f & 0x1) && !(f & 0xC);
}
bool CharacterCustomization::validBase(const CharacterLook &l, int base, int variation,
                                       int color) const {
    return variation >= 0 && color >= 0 && variation < numVariations(l.race, l.sex, base) &&
           color < numColors(l.race, l.sex, base, variation);
}
int CharacterCustomization::facialCount(const CharacterLook &l) const {
    const auto found = facialCounts_.find(key(l.race, l.sex));
    return found == facialCounts_.end() ? 0 : int(found->second);
}
int CharacterCustomization::numSkins(const CharacterLook &l) const {
    int count = 0;
    for (int i = 0; i < numColors(l.race, l.sex, SKIN, 0); ++i)
        if (const auto *s = get(l.race, l.sex, SKIN, 0, i); s && matches(s->flags, l.cls))
            ++count;
    return count;
}
int CharacterCustomization::numFaces(const CharacterLook &l) const {
    int count = 0;
    for (int i = 0; i < numVariations(l.race, l.sex, FACE); ++i)
        if (const auto *s = get(l.race, l.sex, FACE, i, l.skin); s && matches(s->flags, l.cls))
            ++count;
    return count;
}
int CharacterCustomization::numHairStyles(const CharacterLook &l) const {
    int count = 0;
    for (int i = 0; i < numVariations(l.race, l.sex, HAIR); ++i)
        if (const auto *s = get(l.race, l.sex, HAIR, i, l.hairColor); s && matches(s->flags, l.cls))
            ++count;
    return count;
}
int CharacterCustomization::numHairColors(const CharacterLook &l) const {
    int count = 0;
    for (int i = 0; i < numColors(l.race, l.sex, HAIR, l.hairStyle); ++i)
        if (const auto *s = get(l.race, l.sex, HAIR, l.hairStyle, i); s && matches(s->flags, l.cls))
            ++count;
    return count;
}
int CharacterCustomization::numFacialHair(const CharacterLook &l) const {
    const int variations = numVariations(l.race, l.sex, FACIAL);
    if (!variations)
        return facialCount(l);
    int count = 0;
    for (int i = 0; i < variations; ++i)
        if (const auto *s = get(l.race, l.sex, FACIAL, i, l.hairColor); s && matches(s->flags, l.cls))
            ++count;
    return count;
}
int CharacterCustomization::nthSkin(const CharacterLook &l, int index) const {
    int found = 0;
    for (int i = 0; i < numColors(l.race, l.sex, SKIN, 0); ++i)
        if (const auto *s = get(l.race, l.sex, SKIN, 0, i); s && matches(s->flags, l.cls)) {
            if (index == found)
                return s->color;
            ++found;
        }
    return -1;
}
int CharacterCustomization::nthFace(const CharacterLook &l, int index) const {
    int found = 0;
    for (int i = 0; i < numVariations(l.race, l.sex, FACE); ++i)
        if (const auto *s = get(l.race, l.sex, FACE, i, l.skin); s && matches(s->flags, l.cls)) {
            if (index == found)
                return s->variation;
            ++found;
        }
    return -1;
}
int CharacterCustomization::nthHairStyle(const CharacterLook &l, int index) const {
    int found = 0;
    for (int i = 0; i < numVariations(l.race, l.sex, HAIR); ++i)
        if (const auto *s = get(l.race, l.sex, HAIR, i, l.hairColor); s && matches(s->flags, l.cls)) {
            if (index == found)
                return s->variation;
            ++found;
        }
    return -1;
}
int CharacterCustomization::nthHairColor(const CharacterLook &l, int index) const {
    int found = 0;
    for (int i = 0; i < numColors(l.race, l.sex, HAIR, l.hairStyle); ++i)
        if (const auto *s = get(l.race, l.sex, HAIR, l.hairStyle, i); s && matches(s->flags, l.cls)) {
            if (index == found)
                return s->color;
            ++found;
        }
    return -1;
}
// GetNthFacialFeatureIndex.
int CharacterCustomization::nthFacial(const CharacterLook &l, int index) const {
    if (!validBase(l, FACIAL, l.facialHair, l.hairColor))
        return index < facialCount(l) ? index : -1;
    const int count = numFacialHair(l);
    int matched = 0;
    for (int i = 0; i < count; ++i)
        if (const auto *s = get(l.race, l.sex, FACIAL, i, l.hairColor); s && matches(s->flags, l.cls)) {
            if (index == matched)
                return s->variation;
            ++matched;
        }
    return -1;
}
void CharacterCustomization::setSkin(CharacterLook &l, int skin) const {
    if (skin < 0)
        return;
    l.skin = uint8_t(skin);
    setFace(l, l.face); // SetSkinColor -> SetFace(current face)
}
void CharacterCustomization::setFace(CharacterLook &l, int face) const {
    const auto *skin = get(l.race, l.sex, SKIN, 0, l.skin);
    if (skin && (skin->flags & 0x8))
        return;
    if (validBase(l, FACE, face, l.skin))
        l.face = uint8_t(face);
}
void CharacterCustomization::setHairStyle(CharacterLook &l, int style) const {
    if (validBase(l, HAIR, style, l.hairColor))
        l.hairStyle = uint8_t(style);
}
void CharacterCustomization::setHairColor(CharacterLook &l, int color) const {
    if (validBase(l, HAIR, l.hairStyle, color))
        l.hairColor = uint8_t(color);
}
void CharacterCustomization::setFacial(CharacterLook &l, int facial) const {
    if (facial >= 0 && facial <= facialCount(l))
        l.facialHair = uint8_t(facial);
}
void CharacterCustomization::cycle(CharacterLook &l, Customization type, int delta) const {
    if (!delta)
        return;
    const int step = delta > 0 ? 1 : -1;
    auto wrap = [](int v, int n) { return v >= n ? 0 : (v < 0 ? n - 1 : v); };
    auto underwearOk = [&](int skin) {
        const auto *u = get(l.race, l.sex, UNDERWEAR, 0, skin);
        return u && matches(u->flags, l.cls);
    };
    switch (type) {
    case Customization::Skin: {
        const int n = numColors(l.race, l.sex, SKIN, 0);
        if (n <= 0)
            return;
        int skin = wrap(l.skin + step, n);
        if (skin == l.skin)
            return;
        while (true) {
            const auto *s = get(l.race, l.sex, SKIN, 0, skin);
            const auto *f = get(l.race, l.sex, FACE, l.face, skin);
            if (s && matches(s->flags, l.cls) && f && matches(f->flags, l.cls) && underwearOk(skin)) {
                setSkin(l, s->color);
                return;
            }
            skin = wrap(skin + step, n);
            if (skin == l.skin)
                return;
        }
    }
    case Customization::Face: {
        const int n = numVariations(l.race, l.sex, FACE);
        if (n <= 0)
            return;
        int face = wrap(l.face + step, n);
        if (face == l.face)
            return;
        int skin = l.skin;
        while (true) {
            const int colors = numColors(l.race, l.sex, FACE, face);
            bool matched = false;
            for (int i = 0; i < colors; ++i) {
                skin = (i + l.skin) % colors; // colorOffset = previous skin index
                const auto *f = get(l.race, l.sex, FACE, face, skin);
                const auto *s = get(l.race, l.sex, SKIN, 0, skin);
                if (f && matches(f->flags, l.cls) && s && matches(s->flags, l.cls) && underwearOk(skin)) {
                    matched = true;
                    break;
                }
            }
            if (matched)
                break;
            face = wrap(face + step, n);
            if (face == l.face)
                return;
        }
        const auto *f = get(l.race, l.sex, FACE, face, l.skin);
        const auto *s = get(l.race, l.sex, SKIN, 0, l.skin);
        if (!f || !matches(f->flags, l.cls) || !s || !matches(s->flags, l.cls) || !underwearOk(l.skin))
            l.skin = uint8_t(skin);
        setFace(l, face);
        return;
    }
    case Customization::HairStyle: {
        const int n = numVariations(l.race, l.sex, HAIR);
        if (n <= 0)
            return;
        int style = wrap(l.hairStyle + step, n);
        if (style == l.hairStyle)
            return;
        const Section *rec = nullptr;
        while (true) {
            const int colors = numColors(l.race, l.sex, HAIR, style);
            int color = 0;
            for (; color < colors; ++color) {
                rec = get(l.race, l.sex, HAIR, style, color);
                if (rec && matches(rec->flags, l.cls))
                    break;
            }
            if (color < colors)
                break;
            style = wrap(style + step, n);
            if (style == l.hairStyle)
                return;
        }
        if (const auto *keep = get(l.race, l.sex, HAIR, style, l.hairColor);
            keep && matches(keep->flags, l.cls)) {
            setHairStyle(l, keep->variation);
            return;
        }
        l.hairStyle = uint8_t(rec->variation);
        setHairColor(l, rec->color);
        if (const int facial = nthFacial(l, 0); facial >= 0)
            setFacial(l, facial);
        return;
    }
    case Customization::HairColor: {
        const int n = numColors(l.race, l.sex, HAIR, l.hairStyle);
        if (n <= 0)
            return;
        int color = wrap(l.hairColor + step, n);
        if (color == l.hairColor)
            return;
        while (true) {
            const auto *rec = get(l.race, l.sex, HAIR, l.hairStyle, color);
            if (rec && matches(rec->flags, l.cls)) {
                setHairColor(l, rec->color);
                return;
            }
            color = wrap(color + step, n);
            if (color == l.hairColor)
                return;
        }
    }
    case Customization::FacialHair: {
        if (!validBase(l, FACIAL, l.facialHair, l.hairColor)) {
            const int count = facialCount(l);
            int facial = l.facialHair + step;
            if (step > 0 && facial >= count)
                facial = 0;
            if (step < 0 && facial < 0)
                facial = count > 0 ? count - 1 : 0;
            setFacial(l, facial);
            return;
        }
        const int n = numVariations(l.race, l.sex, FACIAL);
        if (n <= 0)
            return;
        int facial = wrap(l.facialHair + step, n);
        if (facial == l.facialHair)
            return;
        const Section *rec = nullptr;
        while (true) {
            const int colors = numColors(l.race, l.sex, FACIAL, facial);
            int color = 0;
            for (; color < colors; ++color) {
                rec = get(l.race, l.sex, FACIAL, facial, color);
                if (rec && matches(rec->flags, l.cls))
                    break;
            }
            if (color < colors)
                break;
            facial = wrap(facial + step, n);
            if (facial == l.facialHair)
                return;
        }
        if (const auto *keep = get(l.race, l.sex, FACIAL, facial, l.hairColor);
            keep && matches(keep->flags, l.cls)) {
            setFacial(l, keep->variation);
            return;
        }
        setHairColor(l, rec->color);
        setFacial(l, rec->variation);
        return;
    }
    }
}
void CharacterCustomization::randomize(CharacterLook &l, std::mt19937 &rng) const {
    auto dice = [&](int n) { return n > 0 ? int(std::uniform_int_distribution<int>(0, n - 1)(rng)) : 0; };
    setSkin(l, nthSkin(l, dice(numSkins(l))));
    if (const int c = nthHairColor(l, dice(numHairColors(l))); c >= 0)
        setHairColor(l, c);
    if (const int s = nthHairStyle(l, dice(numHairStyles(l))); s >= 0)
        setHairStyle(l, s);
    if (const int f = nthFace(l, dice(numFaces(l))); f >= 0)
        setFace(l, f);
    if (const int f = nthFacial(l, dice(numFacialHair(l))); f >= 0)
        setFacial(l, f);
}
void CharacterCustomization::validate(CharacterLook &l) const {
    auto valid = [&](int base, int variation, int color) {
        const auto *s = get(l.race, l.sex, base, variation, color);
        return s && matches(s->flags, l.cls);
    };
    if (!valid(SKIN, 0, l.skin)) {
        const int n = numSkins(l);
        l.skin = uint8_t(n <= 0 ? 0 : std::max(0, nthSkin(l, l.skin % n)));
    }
    if (!valid(FACE, l.face, l.skin)) {
        const int n = numFaces(l);
        l.face = uint8_t(n <= 0 ? 0 : std::max(0, nthFace(l, l.face % n)));
    }
    if (!valid(HAIR, l.hairStyle, l.hairColor)) {
        const int n = numHairStyles(l);
        if (n > 0)
            l.hairStyle = uint8_t(std::max(0, nthHairStyle(l, l.hairStyle % n)));
        else {
            const int variations = numVariations(l.race, l.sex, HAIR);
            std::vector<int> counts(variations);
            int numValid = 0;
            for (int i = 0; i < variations; ++i) {
                CharacterLook probe = l;
                probe.hairStyle = uint8_t(i);
                counts[i] = numHairColors(probe);
                numValid += counts[i] > 0;
            }
            if (numValid <= 0)
                l.hairStyle = l.hairColor = 0;
            else {
                for (int i = 0; i < variations; ++i)
                    if (counts[i] > 0 && l.hairStyle % numValid == i) {
                        l.hairStyle = uint8_t(i);
                        break;
                    }
                if (size_t(l.hairStyle) < counts.size() && counts[l.hairStyle] > 0)
                    l.hairColor = uint8_t(std::max(0, nthHairColor(l, l.hairColor % counts[l.hairStyle])));
            }
        }
    }
    // ComponentValidateFacialFeature
    bool facialOk = l.facialHair <= facialCount(l);
    if (facialOk && validBase(l, FACIAL, l.facialHair, l.hairColor))
        facialOk = valid(FACIAL, l.facialHair, l.hairColor);
    if (!facialOk) {
        const int n = numFacialHair(l);
        l.facialHair = uint8_t(n <= 0 ? 0 : std::max(0, nthFacial(l, l.facialHair % n)));
    }
}
CharacterLook CharacterCustomization::randomCharacter(uint8_t expansion, std::mt19937 &rng) const {
    CharacterLook l;
    l.sex = uint8_t(std::uniform_int_distribution<int>(0, 1)(rng));
    std::vector<const RaceInfo *> allowed;
    for (const auto &r : races_)
        if (r.requiredExpansion <= expansion)
            allowed.push_back(&r);
    if (allowed.empty())
        allowed.push_back(&races_.front());
    l.race = uint8_t(allowed[std::uniform_int_distribution<size_t>(0, allowed.size() - 1)(rng)]->id);
    std::vector<uint32_t> classes;
    for (const auto &c : classesForRace(l.race))
        if (c.requiredExpansion <= expansion)
            classes.push_back(c.id);
    l.cls = uint8_t(classes.empty() ? 1 : classes[std::uniform_int_distribution<size_t>(0, classes.size() - 1)(rng)]);
    randomize(l, rng);
    validate(l);
    return l;
}
std::string CharacterCustomization::modelPath(uint32_t raceId, uint32_t sex) const {
    const auto *r = race(raceId);
    if (!r)
        throw std::invalid_argument("Unknown race");
    const auto display = displayModel_.find(sex ? r->femaleDisplay : r->maleDisplay);
    if (display == displayModel_.end())
        throw std::invalid_argument("Missing CreatureDisplayInfo for race");
    const auto model = modelNames_.find(display->second);
    if (model == modelNames_.end() || model->second.empty())
        throw std::invalid_argument("Missing CreatureModelData for race");
    auto path = model->second;
    const auto dot = path.find_last_of('.');
    if (dot != std::string::npos)
        path.resize(dot);
    return path + ".m2";
}
std::vector<uint32_t> CharacterCustomization::geosets(const CharacterLook &l) const {
    // ComponentData defaults, then SetHairStyle/SetBeardStyle and GeosRenderPrep.
    std::array<uint32_t, 19> g{1, 101, 201, 301, 401, 501, 601, 702, 801, 901,
                               1001, 1101, 1201, 1301, 1401, 1501, 1601, 1701, 1801};
    g[0] = 1;
    for (const auto &h : hairGeosets_)
        if (h.race == l.race && h.sex == l.sex && h.variation == l.hairStyle) {
            g[0] = h.geoset > 0 ? uint32_t(h.geoset) : 1;
            break;
        }
    for (const auto &f : facialStyles_)
        if (f.race == l.race && f.sex == l.sex && f.variation == l.facialHair) {
            g[1] = 100 + f.geoset[0];
            g[3] = 300 + f.geoset[1];
            g[2] = 200 + f.geoset[2];
            g[16] = 1600 + f.geoset[3];
            g[17] = 1700 + f.geoset[4];
            break;
        }
    bool eyeGlow = l.cls == 6;
    if (const auto *face = get(l.race, l.sex, FACE, l.face, l.skin); face && (face->flags & 0x4))
        eyeGlow = true;
    if (eyeGlow)
        g[17] = 1703;
    std::vector<uint32_t> out{0};
    out.insert(out.end(), g.begin(), g.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
CharacterTextureRecipe CharacterCustomization::textures(const CharacterLook &l) const {
    CharacterTextureRecipe recipe;
    auto add = [&](CharacterSection section, const Section *s, int index) {
        if (s && !s->texture[index].empty())
            recipe.layers.push_back({section, s->texture[index]});
    };
    const auto *skin = get(l.race, l.sex, SKIN, 0, l.skin);
    if (skin) {
        recipe.baseSkin = skin->texture[0];
        recipe.extraSkin = skin->texture[1];
    }
    const auto *face = get(l.race, l.sex, FACE, l.face, l.skin);
    const auto *facial = get(l.race, l.sex, FACIAL, l.facialHair, l.hairColor);
    const auto *hair = get(l.race, l.sex, HAIR, l.hairStyle, l.hairColor);
    const auto *underwear = get(l.race, l.sex, UNDERWEAR, 0, l.skin);
    // RenderPrepHL / RenderPrepHU order: face, facial hair, scalp.
    add(HeadLower, face, 0);
    add(HeadLower, facial, 0);
    add(HeadLower, hair, 1);
    add(HeadUpper, face, 1);
    add(HeadUpper, facial, 1);
    add(HeadUpper, hair, 2);
    add(LegUpper, underwear, 0);  // RenderPrepLU
    add(TorsoUpper, underwear, 1); // RenderPrepTU
    // ReplaceHairTexture, including the bald-male (race flag 0x8) and facial-hair fallbacks.
    const auto *r = race(l.race);
    bool useStyleOne = !l.sex && !l.hairStyle && r && (r->flags & 0x8);
    for (const auto &f : facialStyles_)
        if (f.race == l.race && f.sex == l.sex && f.variation == l.facialHair &&
            (f.geoset[0] || f.geoset[1] || f.geoset[2]) &&
            (!hair || hair->texture[1].empty() || hair->texture[2].empty()))
            useStyleOne = true;
    if (hair)
        recipe.hair = hair->texture[0];
    if (useStyleOne || recipe.hair.empty())
        if (const auto *one = get(l.race, l.sex, HAIR, 1, l.hairColor); one && !one->texture[0].empty())
            recipe.hair = one->texture[0];
    return recipe;
}
std::string CharacterCustomization::randomName(uint32_t race, uint32_t sex, std::mt19937 &rng) const {
    // CNameGen::LoadNames + CreateDictionary: trigrams of "_name" (lower case).
    std::map<std::string, int> counts;
    for (const auto &n : names_) {
        if (n.race != int32_t(race) || n.sex != int32_t(sex) || n.name.empty())
            continue;
        const auto name = lower("_" + n.name);
        for (size_t j = 0; j + 1 < name.size(); ++j)
            ++counts[name.substr(j, 3)];
    }
    if (counts.empty())
        return {};
    const std::vector<std::pair<std::string, int>> dictionary(counts.begin(), counts.end());
    for (int attempt = 0; attempt < 200; ++attempt) {
        std::string buffer;
        while (true) {
            // GenerateName: first pick among '_' records, then records starting with the
            // last two letters; a two-letter record terminates the name.
            const std::string prefix = buffer.empty() ? "_" : buffer.substr(buffer.size() - 2);
            int total = 0;
            std::vector<const std::pair<std::string, int> *> candidates;
            for (const auto &entry : dictionary)
                if (entry.first.compare(0, prefix.size(), prefix) == 0) {
                    candidates.push_back(&entry);
                    total += entry.second;
                }
            if (candidates.empty())
                break;
            int roll = std::uniform_int_distribution<int>(0, total - 1)(rng);
            const std::pair<std::string, int> *choice = candidates.front();
            for (const auto *c : candidates) {
                if (roll < c->second) {
                    choice = c;
                    break;
                }
                roll -= c->second;
            }
            if (buffer.empty()) {
                buffer = choice->first;
                continue;
            }
            if (choice->first.size() < 3)
                break;
            buffer += choice->first.substr(2);
            if (buffer.size() >= 13)
                break;
        }
        if (buffer.size() < 2)
            continue;
        std::string name = buffer.substr(1);
        name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
        if (name.size() >= 4 && name.size() <= 10 && characterNameLooksValid(name))
            return name;
    }
    return {};
}
std::string CharacterCustomization::backgroundModel(uint32_t raceId, uint32_t cls) const {
    std::string name;
    if (cls == 6) {
        if (const auto *c = classInfo(6))
            name = c->filename;
    } else {
        if (raceId == 7)
            raceId = 3;
        else if (raceId == 8)
            raceId = 2;
        if (const auto *r = race(raceId))
            name = r->fileString;
    }
    if (name.empty())
        return {};
    return "Interface\\Glues\\Models\\UI_" + name + "\\UI_" + name + ".m2";
}
bool characterNameLooksValid(const std::string &name) {
    size_t letters = 0;
    uint32_t previous = 0, run = 0;
    for (size_t i = 0; i < name.size();) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        uint32_t cp;
        size_t len;
        if (c < 0x80) {
            if (!std::isalpha(c))
                return false;
            cp = uint32_t(std::tolower(c));
            len = 1;
        } else if ((c & 0xe0) == 0xc0) {
            len = 2;
            cp = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {
            len = 3;
            cp = c & 0x0f;
        } else
            return false;
        if (i + len > name.size())
            return false;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(name[i + k]);
            if ((cc & 0xc0) != 0x80)
                return false;
            cp = cp << 6 | (cc & 0x3f);
        }
        run = cp == previous ? run + 1 : 1;
        if (run >= 3)
            return false;
        previous = cp;
        ++letters;
        i += len;
    }
    return letters >= 2 && letters <= 12;
}
} // namespace wow::android
