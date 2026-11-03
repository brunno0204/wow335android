#pragma once
#include "WorldWire.h"
#include <string>
namespace wow::android {
struct CharacterEquipment {
    uint32_t display = 0, visual = 0;
    uint8_t inventoryType = 0;
};
struct CharacterSummary {
    uint64_t guid = 0;
    std::string name; // Owned bounded wire bytes; no Unicode/DBC/name normalization.
    uint8_t race = 0, classId = 0, sex = 0, skin = 0, face = 0, hairStyle = 0, hairColor = 0,
            facialHair = 0, level = 0;
    uint32_t zone = 0, map = 0, guild = 0, flags = 0, customizeFlags = 0;
    std::array<float, 3> position{};
    bool firstLogin = false;
    uint32_t petDisplay = 0, petLevel = 0, petFamily = 0;
    std::array<CharacterEquipment, 23> equipment{};
};
struct CharacterList {
    std::vector<CharacterSummary> characters;
    // Original order: Human,Dwarf,Gnome,NightElf,Draenei,Orc,Troll,Tauren,Undead,BloodElf.
    std::optional<std::array<uint32_t, 10>> raceRestrictions;
};
// Original SMSG_CHAR_ENUM(0x3b), max10 characters, name1..47 bytes and optional
// exact40-byte restriction tail. Nonzero unique GUIDs, finite positions and
// canonical first-login bool required; remaining DBC identifiers/flags stay raw.
CharacterList decodeCharacterList(const WorldPacket &);
} // namespace wow::android
