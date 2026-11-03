#pragma once
#include "CharacterCustomization.h"
#include "TextureDecoder.h"
#include <array>
#include <optional>
#include <vector>
namespace wow::android {
// CCharacterComponent base texture: base skin copied whole, then each layer is scaled
// into its section rectangle with alpha (PasteToSection). Output size = base skin size.
TexturePixels composeCharacterTexture(const TexturePixels &baseSkin,
                                      const std::vector<std::pair<CharacterSection, TexturePixels>> &layers);
using Vec3 = std::array<double, 3>;
struct M2VecTrack {
    std::vector<uint32_t> times;
    std::vector<Vec3> values;
    Vec3 at(uint32_t milliseconds, const Vec3 &base) const; // base + linear key value
};
// Camera 0, attachment positions and sequence 0 length of a Glue ModelFFX model
// (M2Camera/M2Attachment from Engine/Source/Model2/M2Data.h). Spline tangents are
// ignored (linear keys); keys outside the file fall back to the base position.
struct M2SceneInfo {
    bool hasCamera = false;
    float fov = 0.7854f, nearClip = .1f, farClip = 1000;
    Vec3 positionBase{}, targetBase{};
    M2VecTrack position, target;
    std::vector<std::pair<uint32_t, Vec3>> attachments; // id, model-space position
    uint32_t duration = 0;
    std::optional<Vec3> attachment(uint32_t id) const;
};
M2SceneInfo decodeM2Scene(const std::vector<uint8_t> &m2);
// The model's portrait camera (M2Camera type 0, the one SetPortraitTexture renders with):
// model-space eye and target at time 0 and the stored (diagonal) field of view.
struct M2PortraitCamera {
    Vec3 eye{}, target{};
    float fov = .7854f;
};
std::optional<M2PortraitCamera> decodeM2PortraitCamera(const std::vector<uint8_t> &m2);
// Model ModelFFX path in the AccountLogin GlueXML/Lua text, if any.
std::optional<std::string> findGlueModelPath(const std::string &text);
// Glue music sound-kit name (SoundEntries name, e.g. GS_LichKing) referenced by GlueXML Lua.
std::optional<std::string> findGlueMusicName(const std::string &text);
} // namespace wow::android
