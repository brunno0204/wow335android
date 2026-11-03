#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace wow::android {
// ADT object placements (MMDX/MMID/MDDF doodads, MWMO/MWID/MODF map objects).
// Positions are already converted to world space like CMap::CreateDoodadDef /
// CreateMapObjDef: world = center + (-pos.z, -pos.x, pos.y); rotation in radians as
// (rotX = rot.z, rotY = rot.x, rotZ = rot.y) applied Z(+pi), Y, X.
struct TerrainPlacementDef {
    std::string path; // .m2 for doodads (MDX renamed), .wmo for map objects
    uint32_t uniqueId = 0;
    std::array<float, 3> position{}, rotation{}; // world space; radians (X, Y, Z order)
    float scale = 1;
    uint16_t flags = 0, doodadSet = 0;
};
struct TerrainObjects {
    std::vector<TerrainPlacementDef> doodads, mapObjects;
};
TerrainObjects decodeTerrainObjects(const std::vector<uint8_t> &adt);
} // namespace wow::android
