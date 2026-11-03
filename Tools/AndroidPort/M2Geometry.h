#pragma once
#include "Engine/Source/Model2/M2Data.h"
#include <string>
#include <array>
#include <vector>
namespace wow::android {
// Geometry subset of WoW 3.3.5a MD20/v264 + SKIN. Not a full model/animation
// loader or validator. Used section vertices have global bone indices, ready
// for M2Skinning::Transform; triangle indices address this remapped vertex list.
struct M2Geometry {
    uint32_t boneCount = 0;
    std::vector<M2Vertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<M2SkinSection> sections;
    // Global bone index per vertex influence. Models with more than 256 bones (login scenes,
    // large creatures) skin from these instead of the byte palette (wideBones).
    std::vector<std::array<uint16_t, 4>> boneIndices;
    bool wideBones = false;
};
M2Geometry decodeM2Geometry(const std::vector<uint8_t> &model, const std::vector<uint8_t> &skin);
// Uses original synchronous SFile (mounted MPQs or permitted loose paths).
M2Geometry loadM2Geometry(const std::string &modelPath, const std::string &skinPath);
} // namespace wow::android
