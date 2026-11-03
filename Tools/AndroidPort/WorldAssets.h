#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace wow::android {
// Map.dbc (WDBC) record ID -> Directory string (field1), as used for World\Maps\<dir>.
std::string mapDirectory(const std::vector<uint8_t> &mapDbc, uint32_t mapId);
// World XY -> ADT file indices "<dir>_<first>_<second>": first from Y, second from X.
std::pair<uint32_t, uint32_t> terrainTileFor(double x, double y);
struct CharacterModelPaths {
    std::string model, skin, bodyTexture, hairTexture;
};
// Base race/sex player model (Character\<Race>\<Sex>\<Race><Sex>.m2) plus simple
// base skin/hair textures. No CharSections/ItemDisplayInfo composition.
std::optional<CharacterModelPaths> characterModelPaths(uint8_t race, uint8_t sex, uint8_t skin,
                                                       uint8_t hairColor);
// Base geosets: body, default hair/face/ears/feet; no equipment geosets.
std::string characterBaseGeosets();
// First M2 sequence slot (index into the sequence table) for an animation ID.
std::optional<uint32_t> m2SequenceIndex(const std::vector<uint8_t> &m2, uint16_t animationId);
} // namespace wow::android
