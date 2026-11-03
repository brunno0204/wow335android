#pragma once
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include <cstdint>
namespace wow::android {
// GL thread: day/night outdoor light from the client tables (Light.dbc lights of the map,
// LightParams, LightIntBand colours and LightFloatBand fog distances, interpolated like
// DayNight / Lights.cpp of the original), the sky dome and the frame's fog and tint
// (WorldLighting). Not clouds, sun/moon, skybox models or weather.
// Selects the map's lights (loads the tables once).
void worldSkyMap(uint32_t map);
// Updates WorldLighting for the player position and time of day and draws the sky dome
// around the eye (before the opaque world). maxFog: fog end limit (the streamed range).
void worldSkyDraw(const C3Vector &eye, const C3Vector &player, const C44Matrix &view,
                  const C44Matrix &projection, float maxFog);
// After the world pass: lighting off for UI/glue drawing.
void worldSkyEnd();
// Water surface colour (river or ocean shallow band of the frame's light), 0xAARRGGBB.
uint32_t worldSkyWaterColor(bool ocean);
// Clear colour for the next frame (fog colour; CImVector bytes as 0xAARRGGBB).
uint32_t worldSkyClearColor();
// Time of day in half-minutes (0..2879), server game time when known.
void worldSkySetGameTime(uint32_t halfMinutes, uint64_t atMs);
} // namespace wow::android
