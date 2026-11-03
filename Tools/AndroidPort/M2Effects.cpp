// Animated M2 material tracks (texture transforms, colors, texture weights) and particle
// emitters, after Engine/Source/Model2/M2Model.cpp (AnimateParticleST, emitter setup) and
// Engine/Source/Services/ParticleSystem2.cpp (CPlaneParticleEmitter / CSphereParticleEmitter
// ::CreateParticle, MoveParticle, InterpolateColorTrack). Simplified: no tails, twinkle, spin,
// spline emitters, child models or ribbons.
#include "M2Effects.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "Engine/Source/Tempest/CImVector.h"
#include "Engine/Source/Tempest/C2Vector.h"
#include "M2File.h"
#include "WorldLighting.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
namespace wow::android {
namespace {
constexpr size_t kParticleSize = 0x1DC;
float f32At(const std::vector<uint8_t> &b, size_t o) {
    const uint32_t bits = m2::u32(b, o);
    float v;
    std::memcpy(&v, &bits, 4);
    return std::isfinite(v) ? v : 0.f;
}
struct Reader {
    const std::vector<uint8_t> &b;
    size_t header;
    std::vector<uint32_t> sequenceFlags, sequenceDurations, loops;
    uint32_t sequence = 0;
    bool inBounds(uint32_t count, uint32_t offset, size_t element) const {
        return offset >= header && offset <= b.size() && (count == 0 || count <= (b.size() - offset) / element);
    }
    // M2Track<T> at absolute offset: T made of `components` floats (fixed16 when fixed).
    M2EffectTrack track(size_t at, int components, bool fixed16 = false, bool byte = false) const {
        M2EffectTrack t;
        t.components = components;
        t.interpolation = m2::u16(b, at);
        const uint16_t loop = m2::u16(b, at + 2);
        uint32_t index = sequence;
        if (loop != 0xffff) {
            t.global = true;
            index = 0;
            t.loopDuration = loop < loops.size() ? loops[loop] : 0;
        } else {
            if (sequence >= sequenceFlags.size() || !(sequenceFlags[sequence] & 0x20))
                return t; // external .anim data: keep static
            t.loopDuration = sequenceDurations[sequence];
        }
        const uint32_t timesCount = m2::u32(b, at + 4), timesOffset = m2::u32(b, at + 8);
        const uint32_t keysCount = m2::u32(b, at + 12), keysOffset = m2::u32(b, at + 16);
        if (index >= timesCount || index >= keysCount || !inBounds(timesCount, timesOffset, 8) ||
            !inBounds(keysCount, keysOffset, 8))
            return t;
        const uint32_t n = m2::u32(b, timesOffset + 8 * index), timeAt = m2::u32(b, timesOffset + 8 * index + 4);
        const uint32_t k = m2::u32(b, keysOffset + 8 * index), keyAt = m2::u32(b, keysOffset + 8 * index + 4);
        const size_t value = byte ? 1 : fixed16 ? size_t(2 * components) : size_t(4 * components);
        // Hermite/bezier keys may carry in/out tangents after each value (3 values per key).
        const size_t stride = t.interpolation >= 2 && k == 3 * n ? 3 : 1;
        const size_t element = value * stride;
        if (!n || n * stride != k || n > 4096 || !inBounds(n, timeAt, 4) || !inBounds(k, keyAt, value))
            return t;
        t.times.resize(n);
        t.values.resize(size_t(n) * components);
        for (uint32_t i = 0; i < n; ++i) {
            t.times[i] = m2::u32(b, timeAt + 4 * i);
            for (int c = 0; c < components; ++c) {
                float v;
                if (byte)
                    v = float(b[keyAt + element * i]);
                else if (fixed16)
                    v = float(int16_t(m2::u16(b, keyAt + element * i + 2 * c))) / 32767.f;
                else
                    v = f32At(b, keyAt + element * i + 4 * c);
                t.values[size_t(i) * components + c] = v;
            }
        }
        return t;
    }
    M2EffectPartTrack part(size_t at, int components, int kind) const { // kind 0 float, 1 fixed16, 2 u16
        M2EffectPartTrack p;
        p.components = components;
        const uint32_t tn = m2::u32(b, at), to = m2::u32(b, at + 4), vn = m2::u32(b, at + 8), vo = m2::u32(b, at + 12);
        const size_t element = kind == 0 ? size_t(4 * components) : size_t(2 * components);
        if (!tn || tn != vn || tn > 64 || !inBounds(tn, to, 2) || !inBounds(vn, vo, element))
            return p;
        for (uint32_t i = 0; i < tn; ++i) {
            p.times.push_back(float(m2::u16(b, to + 2 * i)) / 32767.f);
            for (int c = 0; c < components; ++c) {
                float v;
                if (kind == 0)
                    v = f32At(b, vo + element * i + 4 * c);
                else if (kind == 1)
                    v = float(int16_t(m2::u16(b, vo + element * i + 2 * c))) / 32767.f;
                else
                    v = float(m2::u16(b, vo + element * i + 2 * c));
                p.values.push_back(v);
            }
        }
        return p;
    }
};
float first(const M2EffectTrack &t, float fallback) { return t.empty() ? fallback : t.values[0]; }
float sample1(const M2EffectTrack &t, uint32_t ms, uint64_t global, float fallback) {
    if (t.empty())
        return fallback;
    float v = fallback;
    t.sample(ms, global, &v);
    return v;
}
} // namespace

void M2EffectTrack::sample(uint32_t sequenceMs, uint64_t globalMs, float *out) const {
    if (empty())
        return;
    uint32_t time = global ? (loopDuration ? uint32_t(globalMs % loopDuration) : 0)
                           : (loopDuration ? sequenceMs % std::max<uint32_t>(1, loopDuration) : sequenceMs);
    const size_t n = times.size();
    if (n == 1 || time <= times[0]) {
        std::copy_n(values.begin(), components, out);
        return;
    }
    if (time >= times[n - 1]) {
        std::copy_n(values.begin() + long((n - 1) * components), components, out);
        return;
    }
    const size_t i = size_t(std::upper_bound(times.begin(), times.end(), time) - times.begin());
    const float span = float(times[i] - times[i - 1]);
    const float k = interpolation == 0 || span <= 0 ? 0.f : float(time - times[i - 1]) / span;
    for (int c = 0; c < components; ++c) {
        const float a = values[(i - 1) * components + c], b = values[i * components + c];
        out[c] = a + (b - a) * k;
    }
}
void M2EffectPartTrack::sample(float t, float *out) const {
    if (times.empty())
        return;
    const size_t n = times.size();
    if (n == 1 || t <= times[0]) {
        std::copy_n(values.begin(), components, out);
        return;
    }
    if (t >= times[n - 1]) {
        std::copy_n(values.begin() + long((n - 1) * components), components, out);
        return;
    }
    size_t i = 1;
    while (i < n && times[i] < t)
        ++i;
    const float span = times[i] - times[i - 1];
    const float k = span > 0 ? (t - times[i - 1]) / span : 0;
    for (int c = 0; c < components; ++c) {
        const float a = values[(i - 1) * components + c], b = values[i * components + c];
        out[c] = a + (b - a) * k;
    }
}
bool M2EffectsData::animatedMaterials() const noexcept {
    auto animated = [](const M2EffectTrack &t) { return t.times.size() > 1; };
    for (const auto &tt : textureTransforms)
        for (const auto &t : tt)
            if (animated(t))
                return true;
    for (const auto &t : colorRgb)
        if (animated(t))
            return true;
    for (const auto &t : colorAlpha)
        if (animated(t))
            return true;
    for (const auto &t : weights)
        if (animated(t))
            return true;
    return false;
}

std::shared_ptr<const M2EffectsData> loadM2Effects(const std::vector<uint8_t> &b, uint32_t sequenceIndex) {
    auto out = std::make_shared<M2EffectsData>();
    const size_t header = m2::modelHeader(b);
    Reader r{b, header, {}, {}, {}, sequenceIndex};
    {
        const uint32_t count = m2::u32(b, 0x1c), offset = m2::u32(b, 0x20);
        if (count <= 4096 && r.inBounds(count, offset, 64))
            for (uint32_t i = 0; i < count; ++i) {
                r.sequenceDurations.push_back(m2::u32(b, offset + 64 * i + 4));
                r.sequenceFlags.push_back(m2::u32(b, offset + 64 * i + 12));
            }
        const uint32_t loops = m2::u32(b, 0x14), loopAt = m2::u32(b, 0x18);
        if (loops <= 4096 && r.inBounds(loops, loopAt, 4))
            for (uint32_t i = 0; i < loops; ++i)
                r.loops.push_back(m2::u32(b, loopAt + 4 * i));
    }
    if (sequenceIndex < r.sequenceDurations.size())
        out->sequenceDuration = r.sequenceDurations[sequenceIndex];
    // Materials.
    {
        const uint32_t count = m2::u32(b, 0x60), offset = m2::u32(b, 0x64);
        if (count <= 1024 && r.inBounds(count, offset, 0x3c))
            for (uint32_t i = 0; i < count; ++i) {
                const size_t at = offset + 0x3c * i;
                out->textureTransforms.push_back({r.track(at, 3), r.track(at + 0x14, 4), r.track(at + 0x28, 3)});
            }
        const uint32_t colors = m2::u32(b, 0x48), colorAt = m2::u32(b, 0x4c);
        if (colors <= 1024 && r.inBounds(colors, colorAt, 0x28))
            for (uint32_t i = 0; i < colors; ++i) {
                out->colorRgb.push_back(r.track(colorAt + 0x28 * i, 3));
                out->colorAlpha.push_back(r.track(colorAt + 0x28 * i + 0x14, 1, true));
            }
        const uint32_t weights = m2::u32(b, 0x58), weightAt = m2::u32(b, 0x5c);
        if (weights <= 1024 && r.inBounds(weights, weightAt, 0x14))
            for (uint32_t i = 0; i < weights; ++i)
                out->weights.push_back(r.track(weightAt + 0x14 * i, 1, true));
    }
    // Particle emitters.
    const uint32_t count = m2::u32(b, 0x128), offset = m2::u32(b, 0x12c);
    if (count && count <= 64 && r.inBounds(count, offset, kParticleSize)) {
        const uint32_t textures = m2::u32(b, 0x50), textureAt = m2::u32(b, 0x54);
        for (uint32_t i = 0; i < count; ++i) {
            const size_t at = offset + kParticleSize * i;
            M2ParticleDef p;
            p.flags = m2::u32(b, at + 4);
            p.position = {f32At(b, at + 8), f32At(b, at + 12), f32At(b, at + 16)};
            p.bone = m2::u16(b, at + 0x14);
            const uint16_t texture = m2::u16(b, at + 0x16);
            p.blend = b[at + 0x28];
            p.emitterType = b[at + 0x29];
            p.rows = std::max<uint16_t>(1, m2::u16(b, at + 0x30));
            p.cols = std::max<uint16_t>(1, m2::u16(b, at + 0x32));
            p.speed = r.track(at + 0x34, 1);
            p.variation = r.track(at + 0x48, 1);
            p.latitude = r.track(at + 0x5c, 1);
            p.longitude = r.track(at + 0x70, 1);
            p.gravity = r.track(at + 0x84, 1);
            p.life = r.track(at + 0x98, 1);
            p.lifeVariation = f32At(b, at + 0xac);
            p.rate = r.track(at + 0xb0, 1);
            p.rateVariation = f32At(b, at + 0xc4);
            p.width = r.track(at + 0xc8, 1);
            p.length = r.track(at + 0xdc, 1);
            p.zsource = r.track(at + 0xf0, 1);
            p.color = r.part(at + 0x104, 3, 0);
            p.alpha = r.part(at + 0x114, 1, 1);
            p.scale = r.part(at + 0x124, 2, 0);
            p.scaleVariation = {f32At(b, at + 0x134), f32At(b, at + 0x138), 0};
            p.headCell = r.part(at + 0x13c, 1, 2);
            p.drag = f32At(b, at + 0x174);
            p.visibility = r.track(at + 0x1c8, 1, false, true);
            if (p.emitterType < 1 || p.emitterType > 2)
                continue; // spline emitters are not simulated
            // Texture: a named model texture (type 0).
            if (texture < textures && r.inBounds(textures, textureAt, 16)) {
                const size_t t = textureAt + 16 * texture;
                const uint32_t type = m2::u32(b, t), flags = m2::u32(b, t + 4);
                const uint32_t n = m2::u32(b, t + 8), o = m2::u32(b, t + 12);
                if (type == 0 && n > 1 && n < 260 && r.inBounds(n, o, 1)) {
                    std::string path(reinterpret_cast<const char *>(b.data() + o), n - 1);
                    try {
                        p.texture = std::make_shared<TexturePixels>(decodeBlp2(m2::readFile(path, 16 * 1024 * 1024)));
                        p.textureFlags = flags;
                    } catch (const std::exception &) {
                    }
                }
            }
            if (p.texture)
                out->particles.push_back(std::move(p));
        }
    }
    return out;
}

C44Matrix m2TextureMatrix(const M2EffectsData &e, uint16_t index, uint32_t sequenceMs, uint64_t globalMs) {
    C44Matrix m(1);
    if (index >= e.textureTransforms.size())
        return m;
    const auto &tt = e.textureTransforms[index];
    float translation[3]{0, 0, 0}, rotation[4]{0, 0, 0, 1}, scaling[3]{1, 1, 1};
    tt[0].sample(sequenceMs, globalMs, translation);
    tt[1].sample(sequenceMs, globalMs, rotation);
    tt[2].sample(sequenceMs, globalMs, scaling);
    // uv' = ((uv - 0.5) * S * R + 0.5) + T (CM2Model texture matrix around the texture centre).
    const float angle = 2.f * std::atan2(rotation[2], rotation[3]);
    const float c = std::cos(angle), s = std::sin(angle);
    m.a0 = c * scaling[0];
    m.a1 = s * scaling[0];
    m.b0 = -s * scaling[1];
    m.b1 = c * scaling[1];
    m.d0 = -.5f * (m.a0 + m.b0) + .5f + translation[0];
    m.d1 = -.5f * (m.a1 + m.b1) + .5f + translation[1];
    return m;
}

namespace {
void particleTexels(EGxTexCommand command, uint32_t, uint32_t, uint32_t, uint32_t, void *user, uint32_t &stride,
                    const void *&texels) {
    if (command != GxTex_Latch)
        return;
    const auto &image = *static_cast<const TexturePixels *>(user);
    stride = image.width * 4;
    texels = image.rgba.data();
}
struct ParticleVertex {
    C3Vector p;
    CImVector c;
    C2Vector uv;
};
} // namespace

M2ParticleRuntime::M2ParticleRuntime(std::shared_ptr<const M2EffectsData> effects) : effects_(std::move(effects)) {
    if (!effects_)
        return;
    for (const auto &def : effects_->particles) {
        Emitter e;
        e.def = &def;
        e.particles.reserve(64);
        emitters_.push_back(std::move(e));
    }
}
M2ParticleRuntime::~M2ParticleRuntime() = default; // textures are shared (draw cache)
void M2ParticleRuntime::update(float dt, const std::vector<C44Matrix> &bones, uint32_t sequenceMs, uint64_t globalMs) {
    dt = std::clamp(dt, 0.f, .1f);
    // s_emitterRotation of CM2Model::AnimateParticleST: emitter X/Y swapped.
    C44Matrix rotation(0);
    rotation.a1 = 1;
    rotation.b0 = -1;
    rotation.c2 = 1;
    rotation.d3 = 1;
    for (auto &e : emitters_) {
        const auto &d = *e.def;
        C44Matrix local(1);
        local.d0 = d.position.x;
        local.d1 = d.position.y;
        local.d2 = d.position.z;
        const C44Matrix bone = d.bone < bones.size() ? bones[d.bone] : C44Matrix(1);
        e.transform = rotation * (local * bone);
        const bool modelSpace = (d.flags & 0x10) != 0; // emitter flag 0x200
        const float speed = sample1(d.speed, sequenceMs, globalMs, 0), variation = sample1(d.variation, sequenceMs, globalMs, 0);
        const float latitude = sample1(d.latitude, sequenceMs, globalMs, 0), longitude = sample1(d.longitude, sequenceMs, globalMs, 0);
        const float gravity = sample1(d.gravity, sequenceMs, globalMs, 0);
        const float life = std::max(.05f, sample1(d.life, sequenceMs, globalMs, 1));
        const float width = sample1(d.width, sequenceMs, globalMs, 0), length = sample1(d.length, sequenceMs, globalMs, 0);
        const float zsource = sample1(d.zsource, sequenceMs, globalMs, 0);
        const bool visible = sample1(d.visibility, sequenceMs, globalMs, 1) > 0;
        float rate = visible ? sample1(d.rate, sequenceMs, globalMs, 0) : 0;
        rate *= 1 + uniform() * d.rateVariation;
        // Age and move the live particles (MoveParticle).
        for (size_t i = 0; i < e.particles.size();) {
            auto &p = e.particles[i];
            p.age += dt;
            if (p.age >= p.life) {
                p = e.particles.back();
                e.particles.pop_back();
                continue;
            }
            p.position.x += p.velocity.x * dt;
            p.position.y += p.velocity.y * dt;
            p.position.z += p.velocity.z * dt - gravity * dt * dt * .5f;
            p.velocity.z -= gravity * dt;
            if (d.drag != 0) {
                const float drag = std::min(1.f, dt * d.drag);
                p.velocity.x -= drag * p.velocity.x;
                p.velocity.y -= drag * p.velocity.y;
                p.velocity.z -= drag * p.velocity.z;
            }
            ++i;
        }
        // Emit (CPlaneParticleEmitter / CSphereParticleEmitter::CreateParticle).
        e.pending += std::max(0.f, rate) * dt;
        constexpr size_t kMax = 320;
        while (e.pending >= 1.f) {
            e.pending -= 1.f;
            if (e.particles.size() >= kMax)
                continue;
            Particle p;
            C3Vector position{0, 0, 0}, velocity{0, 0, 0};
            const float v = (uniform() * variation + 1.f) * speed;
            if (d.emitterType == 1) {
                position = {uniform() * width * .5f, uniform() * length * .5f, 0};
                if (zsource == 0) {
                    const float lat = uniform() * latitude, lon = uniform() * longitude;
                    velocity = {std::cos(lon) * std::sin(lat) * v, std::sin(lat) * std::sin(lon) * v, std::cos(lat) * v};
                } else {
                    C3Vector dir{position.x, position.y, position.z - zsource};
                    const float n = std::max(1e-4f, std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z));
                    velocity = {dir.x / n * v, dir.y / n * v, dir.z / n * v};
                }
            } else {
                const float radius = (uniform() * .5f + .5f) * (length - width) + width;
                const float lat = uniform() * latitude, lon = uniform() * longitude;
                const C3Vector dir{std::cos(lon) * std::cos(lat), std::cos(lat) * std::sin(lon), std::sin(lat)};
                position = {dir.x * radius, dir.y * radius, dir.z * radius};
                C3Vector heading = dir;
                if (zsource != 0) {
                    heading = {position.x, position.y, position.z - zsource};
                    const float n = std::max(1e-4f, std::sqrt(heading.x * heading.x + heading.y * heading.y + heading.z * heading.z));
                    heading = {heading.x / n, heading.y / n, heading.z / n};
                } else if (d.flags & 0x100)
                    heading = {0, 0, 1};
                velocity = {heading.x * v, heading.y * v, heading.z * v};
            }
            if (!modelSpace) {
                const auto &t = e.transform;
                velocity = {t.a0 * velocity.x + t.b0 * velocity.y + t.c0 * velocity.z,
                            t.a1 * velocity.x + t.b1 * velocity.y + t.c1 * velocity.z,
                            t.a2 * velocity.x + t.b2 * velocity.y + t.c2 * velocity.z};
                position = position * t;
            }
            p.position = position;
            p.velocity = velocity;
            p.life = std::max(.05f, life + uniform() * d.lifeVariation);
            p.scaleRandom = 1 + uniform() * d.scaleVariation.x;
            e.particles.push_back(p);
        }
    }
}
void M2ParticleRuntime::draw(const C44Matrix &view, const C44Matrix &projection) {
    // Camera axes for billboards (row-vector view: columns of the rotation).
    const C3Vector right{view.a0, view.b0, view.c0}, up{view.a1, view.b1, view.c1};
    std::vector<ParticleVertex> vertices;
    std::vector<uint16_t> indices;
    struct ProjectionMode {
        int32_t previous = GxMasterEnable(GxMasterEnable_NormalProjection);
        ~ProjectionMode() { GxMasterEnableSet(GxMasterEnable_NormalProjection, previous); }
    } projectionMode;
    GxMasterEnableSet(GxMasterEnable_NormalProjection, 1);
    GxXformSetProjection(projection);
    GxXformSetView(view);
    GxXformSet(GxXform_World, C44Matrix(1));
    GxXformSet(GxXform_Tex0, C44Matrix(1));
    GxRsSet(GxRs_Lighting, 0);
    applyWorldFog();
    GxRsSet(GxRs_Texture1, static_cast<void *>(nullptr));
    GxRsSet(GxRs_TextureShader0, 0);
    GxRsSet(GxRs_TextureShader1, 0);
    GxRsSet(GxRs_TexGen0, GxTexGen_Disabled);
    GxRsSet(GxRs_TexGen1, GxTexGen_Disabled);
    GxRsSet(GxRs_ColorOp0, GxTexOp_Mod);
    GxRsSet(GxRs_AlphaOp0, GxTexOp_Mod);
    GxRsSet(GxRs_ColorOp1, GxTexOp_PassThru);
    GxRsSet(GxRs_AlphaOp1, GxTexOp_PassThru);
    GxRsSet(GxRs_TextureCoord0, 0);
    GxRsSet(GxRs_MatDiffuse, int32_t(0xffffffffu));
    GxRsSet(GxRs_DepthTest, 1);
    GxRsSet(GxRs_DepthWrite, 0);
    GxRsSet(GxRs_Culling, 0);
    for (auto &e : emitters_) {
        if (e.particles.empty())
            continue;
        const auto &d = *e.def;
        if (!e.texture) {
            // Shared by every emitter using this image (kept for the process lifetime).
            static std::unordered_map<const TexturePixels *, std::pair<std::shared_ptr<const TexturePixels>, CGxTex *>> cache;
            auto &slot = cache[d.texture.get()];
            if (!slot.second) {
                const auto &image = *d.texture;
                if (!GxTexCreate(image.width, image.height, GxTex_Abgr8888, CGxTexFlags(GxTex_Linear),
                                 const_cast<TexturePixels *>(&image), particleTexels, slot.second)) {
                    cache.erase(d.texture.get());
                    continue;
                }
                slot.first = d.texture;
            }
            e.texture = slot.second;
        }
        static const int blends[7]{GxBlend_Opaque, GxBlend_AlphaKey, GxBlend_Alpha, GxBlend_NoAlphaAdd, GxBlend_Add,
                                   GxBlend_Mod, GxBlend_Mod2x};
        GxRsSet(GxRs_BlendingMode, blends[std::min<uint8_t>(d.blend, 6)]);
        GxRsSet(GxRs_AlphaRef, d.blend == 1 ? 128 : 0);
        GxRsSet(GxRs_Texture0, e.texture);
        vertices.clear();
        indices.clear();
        const bool modelSpace = (d.flags & 0x10) != 0;
        const uint32_t cells = uint32_t(d.rows) * d.cols;
        for (const auto &p : e.particles) {
            if (vertices.size() + 4 > 65000)
                break;
            const float t = std::clamp(p.age / p.life, 0.f, 1.f);
            float rgb[3]{255, 255, 255}, alpha = 1, scale[2]{1, 1}, cell = 0;
            d.color.sample(t, rgb);
            d.alpha.sample(t, &alpha);
            d.scale.sample(t, scale);
            d.headCell.sample(t, &cell);
            const float sx = scale[0] * p.scaleRandom, sy = scale[1] * p.scaleRandom;
            const C3Vector center = modelSpace ? p.position * e.transform : p.position;
            CImVector color;
            color.r = uint8_t(std::clamp(rgb[0], 0.f, 255.f));
            color.g = uint8_t(std::clamp(rgb[1], 0.f, 255.f));
            color.b = uint8_t(std::clamp(rgb[2], 0.f, 255.f));
            color.a = uint8_t(std::clamp(alpha, 0.f, 1.f) * 255);
            const uint32_t index = cells ? uint32_t(cell) % cells : 0;
            const float u0 = float(index % d.cols) / d.cols, v0 = float(index / d.cols) / d.rows;
            const float u1 = u0 + 1.f / d.cols, v1 = v0 + 1.f / d.rows;
            const auto base = uint16_t(vertices.size());
            auto corner = [&](float a, float b, float u, float v) {
                vertices.push_back({{center.x + right.x * a * sx + up.x * b * sy, center.y + right.y * a * sx + up.y * b * sy,
                                     center.z + right.z * a * sx + up.z * b * sy},
                                    color, {u, v}});
            };
            corner(-1, 1, u0, v0);
            corner(1, 1, u1, v0);
            corner(1, -1, u1, v1);
            corner(-1, -1, u0, v1);
            indices.insert(indices.end(), {base, uint16_t(base + 1), uint16_t(base + 2), base, uint16_t(base + 2),
                                           uint16_t(base + 3)});
        }
        if (indices.empty())
            continue;
        GxPrimVertexPtr(vertices.size(), &vertices[0].p, sizeof(ParticleVertex), nullptr, 0, &vertices[0].c,
                        sizeof(ParticleVertex), &vertices[0].uv, sizeof(ParticleVertex), nullptr, 0);
        GxPrimIndexPtr(indices.size(), indices.data());
        CGxBatch batch{GxPrim_Triangles, 0, uint32_t(indices.size()), 0, uint16_t(vertices.size() - 1)};
        GxDraw(&batch, 1);
    }
    GxRsSet(GxRs_DepthWrite, 1);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_AlphaRef, 0);
}
} // namespace wow::android
