#pragma once
#include "ClientDb.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>
namespace wow::android {
// Character appearance rules ported from the original client:
// WoW/Common/ComponentCore (CCharacterComponent, ComponentUtils) and
// WoW/Source/Glue/CharacterCreation (race/class lists, randomization, NameGen).
// Creation context is the original COMPONENT_CONTEXT_0.
struct CharacterLook {
    uint8_t race = 1, cls = 1, sex = 0, skin = 0, face = 0, hairStyle = 0, hairColor = 0,
            facialHair = 0;
};
struct RaceInfo {
    uint32_t id = 0, flags = 0, requiredExpansion = 0, maleDisplay = 0, femaleDisplay = 0;
    int faction = 0; // 0 Alliance, 1 Horde (original FactionGroup internal names)
    std::string clientPrefix, fileString, name, nameMale, nameFemale, hairToken;
    std::array<std::string, 2> facialToken;
};
struct ClassInfo {
    uint32_t id = 0, requiredExpansion = 0;
    std::string name, nameMale, nameFemale, filename;
};
enum class Customization { Skin, Face, HairStyle, HairColor, FacialHair };
// Section indices of the original COMPONENT_SECTIONS.
enum CharacterSection : uint8_t {
    ArmUpper, ArmLower, Hand, TorsoUpper, TorsoLower, LegUpper, LegLower, Foot, HeadUpper,
    HeadLower
};
struct TextureLayer {
    CharacterSection section;
    std::string path;
};
struct CharacterTextureRecipe {
    std::string baseSkin, extraSkin, hair; // M2 texture types 1, 8, 6
    std::vector<TextureLayer> layers;      // pasted in order over the base skin
};
// Pixel rectangle of a section on a composed texture of the given size (base 512).
std::array<uint32_t, 4> characterSectionRect(CharacterSection, uint32_t width, uint32_t height);
struct CharacterDbcFiles {
    std::vector<uint8_t> chrRaces, chrClasses, charBaseInfo, charSections, charHairGeosets,
        facialHairStyles, creatureDisplayInfo, creatureModelData, factionTemplate, factionGroup,
        nameGen;
};
// Logical archive paths, same order as CharacterDbcFiles members.
const std::array<const char *, 11> &characterDbcPaths();
class CharacterCustomization {
  public:
    explicit CharacterCustomization(const CharacterDbcFiles &files);
    // Original CCharacterCreation::Initialize order: Alliance races, then Horde.
    const std::vector<RaceInfo> &races() const noexcept { return races_; }
    const RaceInfo *race(uint32_t id) const;
    const ClassInfo *classInfo(uint32_t id) const;
    std::vector<ClassInfo> classesForRace(uint32_t race) const; // CharBaseInfo order
    bool raceClassValid(uint32_t race, uint32_t cls) const;
    int numSkins(const CharacterLook &) const;
    int numFaces(const CharacterLook &) const;
    int numHairStyles(const CharacterLook &) const;
    int numHairColors(const CharacterLook &) const;
    int numFacialHair(const CharacterLook &) const;
    // Original SetNext*/SetPrev* behaviour for the creation context.
    void cycle(CharacterLook &, Customization, int delta) const;
    // Original InitCharacterComponents(randomize) order: skin, hair color, style, face, facial.
    void randomize(CharacterLook &, std::mt19937 &rng) const;
    // Original ValidateComponentData.
    void validate(CharacterLook &) const;
    // Random race/sex/class honoring the account expansion (GetRandomRaceAndSex/ClassID).
    CharacterLook randomCharacter(uint8_t expansion, std::mt19937 &rng) const;
    std::string modelPath(uint32_t race, uint32_t sex) const; // .m2 via CreatureModelData
    std::vector<uint32_t> geosets(const CharacterLook &) const;
    CharacterTextureRecipe textures(const CharacterLook &) const;
    // Original CNameGen Markov generator; 4..10 letters as GetRandomName requires.
    std::string randomName(uint32_t race, uint32_t sex, std::mt19937 &rng) const;
    // GetCreateBackgroundModel/GetSelectBackgroundModel -> Glue scene model path.
    std::string backgroundModel(uint32_t race, uint32_t cls) const;

  private:
    struct Section {
        int32_t race = 0, sex = 0, base = 0, flags = 0, variation = 0, color = 0;
        std::array<std::string, 3> texture;
    };
    using ColorList = std::vector<const Section *>;
    using VariationList = std::vector<ColorList>;
    const Section *get(uint32_t race, uint32_t sex, int base, int variation, int color) const;
    int numVariations(uint32_t race, uint32_t sex, int base) const;
    int numColors(uint32_t race, uint32_t sex, int base, int variation) const;
    bool matches(int32_t flags, uint8_t cls) const;
    int facialCount(const CharacterLook &) const;
    int nthSkin(const CharacterLook &, int index) const;
    int nthFace(const CharacterLook &, int index) const;
    int nthHairStyle(const CharacterLook &, int index) const;
    int nthHairColor(const CharacterLook &, int index) const;
    int nthFacial(const CharacterLook &, int index) const;
    bool validBase(const CharacterLook &, int base, int variation, int color) const;
    void setSkin(CharacterLook &, int skin) const;
    void setFace(CharacterLook &, int face) const;
    void setHairStyle(CharacterLook &, int style) const;
    void setHairColor(CharacterLook &, int color) const;
    void setFacial(CharacterLook &, int facial) const;
    std::vector<Section> sections_;
    std::map<uint32_t, std::array<VariationList, 5>> lookup_; // race*2+sex
    std::map<uint32_t, uint32_t> facialCounts_;
    struct FacialStyle {
        int32_t race, sex, variation;
        std::array<int32_t, 5> geoset;
    };
    std::vector<FacialStyle> facialStyles_;
    struct HairGeoset {
        int32_t race, sex, variation, geoset, scalp;
    };
    std::vector<HairGeoset> hairGeosets_;
    std::vector<RaceInfo> races_, allRaces_;
    std::vector<ClassInfo> classes_;
    std::vector<std::pair<uint8_t, uint8_t>> baseInfo_;
    std::map<uint32_t, uint32_t> displayModel_;     // CreatureDisplayInfo -> model id
    std::map<uint32_t, std::string> modelNames_;    // CreatureModelData -> path
    struct Name {
        std::string name;
        int32_t race, sex;
    };
    std::vector<Name> names_;
};
// Simplified original name rules: 2..12 letters, letters only (UTF-8 allowed), no three
// identical consecutive letters. The server performs the authoritative validation.
bool characterNameLooksValid(const std::string &name);
} // namespace wow::android
