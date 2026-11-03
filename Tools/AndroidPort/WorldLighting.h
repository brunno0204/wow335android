#pragma once
#include "Engine/Source/Gx/Gx.h"
#include <cstdint>
namespace wow::android {
// GL thread: outdoor light of the current frame (Light.dbc / LightParams / LightIntBand /
// LightFloatBand, see WorldSky.cpp), applied by the terrain, WMO and M2 draws. Off outside the
// world pass (glue scenes, UI model frames keep their own look).
struct WorldLighting {
    bool active = false;
    bool fog = false;
    float fogStart = 0, fogEnd = 0;
    uint32_t fogColor = 0xff808080u; // CImVector bytes (b, g, r, a) as u32
    // Diffuse tint for unlit draws (ambient + sun), 0..255 per channel.
    uint8_t r = 255, g = 255, b = 255;
};
inline WorldLighting &worldLighting() {
    static WorldLighting lighting;
    return lighting;
}
// argb (0xAARRGGBB, as GxRs_MatDiffuse) modulated by the frame's light.
inline uint32_t worldLit(uint32_t argb) {
    const auto &l = worldLighting();
    if (!l.active)
        return argb;
    const uint32_t r = ((argb >> 16) & 0xff) * l.r / 255, g = ((argb >> 8) & 0xff) * l.g / 255,
                   b = (argb & 0xff) * l.b / 255;
    return (argb & 0xff000000u) | r << 16 | g << 8 | b;
}
inline int32_t worldFog() {
    const auto &l = worldLighting();
    return l.active && l.fog ? 1 : 0;
}
// Sets the frame's fog render states (off when no world light is active).
inline void applyWorldFog() {
    const auto &l = worldLighting();
    const bool on = l.active && l.fog && l.fogEnd > l.fogStart + 1;
    GxRsSet(GxRs_Fog, on ? 1 : 0);
    if (on) {
        GxRsSet(GxRs_FogStart, l.fogStart);
        GxRsSet(GxRs_FogEnd, l.fogEnd);
        GxRsSet(GxRs_FogColor, int32_t(l.fogColor));
    }
}
} // namespace wow::android
