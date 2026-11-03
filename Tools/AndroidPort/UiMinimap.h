#pragma once
#include <cstdint>
namespace wow::android {
// GL thread. Lua functions (__AndroidMinimapAttach, __AndroidMinimapZoom, __AndroidPlayerPose).
void uiMinimapRegister();
// Recomposes the minimap texture when the player moved/zoomed (throttled).
void uiMinimapUpdate(uint64_t nowMs);
void uiMinimapReset();
// Player pose provider (default: the world scene); tests and tools can substitute one.
void uiMinimapSetPoseSource(bool (*source)(uint32_t &map, double &x, double &y, double &z, double &heading));
} // namespace wow::android
