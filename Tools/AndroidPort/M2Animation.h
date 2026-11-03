#pragma once
#include "Engine/Source/Tempest/C44Matrix.h"
#include <cstdint>
#include <cstddef>
#include <utility>
#include <memory>
#include <string>
#include <vector>
namespace wow::android {
struct M2AnimationData;
struct M2AnimationClock {
    uint64_t milliseconds = 0;
    bool loop = true;
};
// Immutable selected MD20/v264 sequence, embedded or external .anim.
// Alias chains choose the resource; requested track slot/duration are retained.
// Basic step/linear TRS and global loops. Billboards, inheritance overrides,
// and spline tracks are not implemented.
class M2Animation {
  public:
    M2Animation() = default;
    bool initialized() const noexcept;
    uint32_t duration() const;
    uint32_t blendTime() const;
    size_t boneCount() const;
    // World transforms each root; do not apply it again to the skinned draw.
    // sequenceTime/globalTime are independent millisecond clocks.
    std::vector<C44Matrix> evaluate(uint64_t sequenceTime, uint64_t globalTime, bool loop = true,
                                    const C44Matrix &world = C44Matrix(1)) const;

    // Blend LOCAL channels before composing parents/pivots. 0=primary,
    // 1=secondary. Primary step/global channels bypass cross-sequence blending.
    // Clips must have matching bone/track metadata; durations/keys may differ.
    std::vector<C44Matrix> evaluateBlended(const M2Animation &secondary, M2AnimationClock primaryTime,
                                           M2AnimationClock secondaryTime, uint64_t globalTime,
                                           float secondaryWeight,
                                           const C44Matrix &world = C44Matrix(1)) const;
    std::vector<C44Matrix> evaluateBlended(const M2Animation &secondary, M2AnimationClock primaryTime,
                                           M2AnimationClock secondaryTime, uint64_t globalTime,
                                           const std::vector<float> &secondaryWeights,
                                           const C44Matrix &world = C44Matrix(1)) const;

  private:
    std::shared_ptr<const M2AnimationData> data;
    explicit M2Animation(std::shared_ptr<const M2AnimationData> value) : data(std::move(value)) {}
    friend M2Animation decodeM2Animation(const std::vector<uint8_t> &, uint32_t);
    friend M2Animation decodeM2Animation(const std::vector<uint8_t> &, uint32_t,
                                         const std::vector<uint8_t> &);
    friend M2Animation loadM2Animation(const std::string &, uint32_t);
    friend std::vector<M2Animation> loadM2Animations(const std::string &, const std::vector<uint8_t> &,
                                                   const std::vector<uint32_t> &);
};
M2Animation decodeM2Animation(const std::vector<uint8_t> &model, uint32_t sequenceIndex = 0);
// Explicit resource bytes cover the requested slot and aliases of the same
// resolved resource. The SFile loader can resolve distinct fallback resources.
M2Animation decodeM2Animation(const std::vector<uint8_t> &model, uint32_t sequenceIndex,
                              const std::vector<uint8_t> &animation);
// Reads companion <model-base><id:04>-<variation:02>.anim lazily through SFile.
M2Animation loadM2Animation(const std::string &path, uint32_t sequenceIndex = 0);
// Decode up to 32 distinct slots from ONE model snapshot. Path is used only
// to locate external resources. The call shares a bounded external-file cache;
// returned clips own their decoded data and require no subsequent file IO.
std::vector<M2Animation> loadM2Animations(const std::string &path, const std::vector<uint8_t> &model,
                                        const std::vector<uint32_t> &sequenceIndices);
} // namespace wow::android
