#pragma once
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "TextureDecoder.h"
#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>
class CGxTex;
namespace wow::android {
// One sampled M2Track<T> (T = 1..4 floats) for one sequence or its global loop.
struct M2EffectTrack {
    int components = 0;
    uint16_t interpolation = 0;
    bool global = false;         // global loop (m_loopIndex)
    uint32_t loopDuration = 0;   // global loop length / sequence duration
    std::vector<uint32_t> times;
    std::vector<float> values;   // components per key
    bool empty() const noexcept { return times.empty() || values.empty(); }
    // Value at the sequence time (or the global time for global loops).
    void sample(uint32_t sequenceMs, uint64_t globalMs, float *out) const;
};
// M2PartTrack (particle "fake" track): keys over the particle lifetime (0..1).
struct M2EffectPartTrack {
    int components = 0;
    std::vector<float> times; // 0..1
    std::vector<float> values;
    void sample(float t, float *out) const;
};
struct M2ParticleDef {
    uint32_t flags = 0;
    C3Vector position{};
    uint16_t bone = 0;
    uint8_t blend = 0, emitterType = 1;
    uint16_t rows = 1, cols = 1;
    M2EffectTrack speed, variation, latitude, longitude, gravity, life, rate, width, length, zsource, visibility;
    float lifeVariation = 0, rateVariation = 0;
    M2EffectPartTrack color, alpha, scale, headCell;
    C3Vector scaleVariation{};
    float drag = 0, spin = 0, spinVariation = 0;
    std::shared_ptr<const TexturePixels> texture;
    uint32_t textureFlags = 0;
};
// Animated material data and particle emitters of one model for one sequence (inline data
// only; external .anim sequences keep the static look).
struct M2EffectsData {
    uint32_t sequenceDuration = 0;
    std::vector<std::array<M2EffectTrack, 3>> textureTransforms; // translation, rotation, scaling
    std::vector<M2EffectTrack> colorRgb, colorAlpha, weights;
    std::vector<M2ParticleDef> particles;
    bool animatedMaterials() const noexcept;
};
// Asset thread (archive mounted): particle textures are read and decoded here.
std::shared_ptr<const M2EffectsData> loadM2Effects(const std::vector<uint8_t> &model, uint32_t sequenceIndex);
// UV matrix of a texture transform at the given time (row-vector, for GxXform_Tex0).
C44Matrix m2TextureMatrix(const M2EffectsData &effects, uint16_t transform, uint32_t sequenceMs, uint64_t globalMs);

// GL thread: particles of one placed model (original CParticleEmitter2 behaviour, simplified).
class M2ParticleRuntime {
  public:
    explicit M2ParticleRuntime(std::shared_ptr<const M2EffectsData> effects);
    ~M2ParticleRuntime();
    M2ParticleRuntime(const M2ParticleRuntime &) = delete;
    M2ParticleRuntime &operator=(const M2ParticleRuntime &) = delete;
    bool empty() const noexcept { return emitters_.empty(); }
    // bones: current pose in world space (M2AnimatedModel::bones(world)).
    void update(float seconds, const std::vector<C44Matrix> &bones, uint32_t sequenceMs, uint64_t globalMs);
    void draw(const C44Matrix &view, const C44Matrix &projection);

  private:
    struct Particle {
        C3Vector position, velocity;
        float age = 0, life = 1, scaleRandom = 1, spin = 0;
    };
    struct Emitter {
        const M2ParticleDef *def = nullptr;
        std::vector<Particle> particles;
        float pending = 0;
        C44Matrix transform{1};
        CGxTex *texture = nullptr;
    };
    std::shared_ptr<const M2EffectsData> effects_;
    std::vector<Emitter> emitters_;
    std::mt19937 random_{12345};
    float uniform() { return std::uniform_real_distribution<float>(-1.f, 1.f)(random_); }
};
} // namespace wow::android
