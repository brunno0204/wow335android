#include "M2Animation.h"
#include "M2File.h"
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Tempest/C4Quaternion.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace wow::android {
namespace {
using m2::require;
template <class T> struct Track {
    bool step = true, global = false;
    uint32_t period = 0;
    uint32_t sequenceCount = 0;
    uint16_t loopIndex = 0xffff;
    std::vector<uint32_t> times;
    std::vector<T> values;
};
struct Bone {
    uint16_t parent = 0xffff;
    bool animated = false;
    uint32_t id = 0, flags = 0, nameCRC = 0;
    C3Vector pivot;
    Track<C3Vector> translation, scale;
    Track<C4Quaternion> rotation;
};
void finite(const C3Vector &v) {
    require(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z), "Nonfinite M2 animation vector");
}
void finite(const C44Matrix &m) {
    float f[16];
    static_assert(sizeof(m) == sizeof(f));
    std::memcpy(f, &m, sizeof(f));
    for (float v : f)
        require(std::isfinite(v), "Nonfinite M2 bone pose");
}
void valid(const C4Quaternion &q) {
    float d = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    require(std::isfinite(d) && d > 1e-6f, "Degenerate M2 quaternion");
}
struct Budget {
    size_t descriptors = 0, keys = 0;
    void outer(size_t n) {
        descriptors += n;
        require(descriptors <= 1024 * 1024, "M2 animation descriptor budget exceeded");
    }
    void inner(size_t n) {
        keys += n;
        require(keys <= 1024 * 1024, "M2 animation key budget exceeded");
    }
};
// Aliases choose the resource file only. The requested sequence retains its
// own duration and track slot, as in CM2Shared::LoadLowPrioritySequence and
// FinishLoadingLowPrioritySequence's per-alias relocation.
struct Metadata {
    size_t header;
    std::vector<M2Sequence> sequences;
    std::vector<int32_t> resolved;
    std::vector<uint8_t> visiting;
    explicit Metadata(const std::vector<uint8_t> &bytes)
        : header(m2::modelHeader(bytes)), sequences(m2::array<M2Sequence>(bytes, 0x1c, 4096, header)),
          resolved(sequences.size(), -1), visiting(sequences.size()) {}
    uint32_t target(uint32_t index) {
        require(index < sequences.size(), "M2 animation sequence index exceeds table");
        uint32_t current = index;
        std::vector<uint32_t> path;
        while (resolved[current] < 0) {
            require(!visiting[current], "Cyclic M2 animation aliases");
            visiting[current] = 1;
            path.push_back(current);
            if (!(sequences[current].m_flags & 0x40)) {
                resolved[current] = current;
                break;
            }
            current = sequences[current].m_aliasNext;
            require(current < sequences.size(), "M2 animation alias exceeds sequence table");
        }
        const auto result = uint32_t(resolved[current]);
        for (auto member : path) {
            resolved[member] = result;
            visiting[member] = 0;
        }
        return result;
    }
    std::pair<uint16_t, uint16_t> resource(uint32_t index) {
        const auto &source = sequences[target(index)];
        return {source.m_id, source.m_variationIndex};
    }
};
using ExternalSource = std::function<const std::vector<uint8_t> &(uint32_t)>;
std::string animationPath(const std::string &model, std::pair<uint16_t, uint16_t> resource) {
    auto base = model;
    auto slash = base.find_last_of("/\\"), dot = base.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        base.resize(dot);
    auto padded = [](uint16_t number, size_t width) {
        auto text = std::to_string(number);
        if (text.size() < width)
            text.insert(0, width - text.size(), '0');
        return text;
    };
    return base + padded(resource.first, 4) + "-" + padded(resource.second, 2) + ".anim";
}

// Layout and arithmetic follow M2Model.cpp FindKey/M2AnimateTrack and
// AnimateBones: vector lerp, packed quaternion -> Tempest Nlerp, pivoted TRS.
// Raw records are read with bounds checks, without relocating file offsets.
template <class Raw, class Out, class Convert>
Track<Out> track(const std::vector<uint8_t> &b, size_t at, size_t header, uint32_t sequence,
                 const std::vector<M2Loop> &loops, const std::vector<M2Sequence> &sequences,
                 const ExternalSource &external, Budget &budget, Convert convert) {
    Track<Out> out;
    uint16_t type = m2::u16(b, at), loop = m2::u16(b, at + 2);
    if (type > 1)
        return out; // Bezier/Hermite keys (rare on bones): keep the bind pose for this channel
    out.step = type == 0;
    out.loopIndex = loop;
    auto nt = m2::u32(b, at + 4), nk = m2::u32(b, at + 12);
    budget.outer(size_t(nt) + nk);
    auto times = m2::array<M2Array<uint32_t>>(b, at + 4, 4096, header);
    auto keys = m2::array<M2Array<Raw>>(b, at + 12, 4096, header);
    require(times.size() == keys.size(), "M2 animation sequence table mismatch");
    out.sequenceCount = times.size();
    if (loop != 0xffff) {
        require(loop < loops.size(), "M2 global loop index exceeds table");
        out.global = true;
        out.period = loops[loop].m_length;
    }
    if (times.empty())
        return out;
    uint32_t selected = out.global ? 0 : (sequence < times.size() ? sequence : 0);
    const auto &t = times[selected];
    const auto &k = keys[selected];
    require(t.m_count == k.m_count, "M2 animation key/time count mismatch");
    budget.inner(size_t(t.m_count) + k.m_count);
    if (!t.m_count)
        return out;
    // Outer tables always live in the model. Non-global inner offsets are
    // relative to the selected slot's embedded model or external resource.
    const std::vector<uint8_t> *bytes = &b;
    size_t dataHeader = header;
    if (!out.global) {
        require(selected < sequences.size(), "M2 animation track slot exceeds sequences");
        if (!(sequences[selected].m_flags & 0x20)) {
            bytes = &external(selected);
            dataHeader = 0;
        }
    }
    require(bytes->size() <= 128 * 1024 * 1024, "M2 animation resource exceeds budget");
    out.times = m2::records<uint32_t>(*bytes, t.m_count, t.m_offset, 1024 * 1024, dataHeader);
    auto raw = m2::records<Raw>(*bytes, k.m_count, k.m_offset, 1024 * 1024, dataHeader);
    require(std::is_sorted(out.times.begin(), out.times.end()), "Unsorted M2 animation times");
    out.values.reserve(raw.size());
    for (const auto &value : raw)
        out.values.push_back(convert(value));
    return out;
}
template <class T, class Lerp>
T sample(const Track<T> &t, uint32_t time, uint64_t global, const T &fallback, Lerp lerp) {
    if (t.times.empty())
        return fallback;
    if (t.global)
        time = t.period ? uint32_t(global % t.period) : 0;
    if (time < t.times.front())
        return t.values.front(); // Clamp instead of unsigned time underflow.
    auto end = std::upper_bound(t.times.begin(), t.times.end(), time);
    size_t i = size_t(end - t.times.begin()) - 1;
    if (t.step || i + 1 == t.times.size())
        return t.values[i];
    float factor = float(time - t.times[i]) / float(t.times[i + 1] - t.times[i]);
    return lerp(factor, t.values[i], t.values[i + 1]);
}
C3Vector lerp(float t, const C3Vector &a, const C3Vector &b) {
    C3Vector v{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    finite(v);
    return v;
}
C4Quaternion qlerp(float t, const C4Quaternion &a, const C4Quaternion &b) {
    auto q = C4Quaternion::Nlerp(t, a, b);
    valid(q);
    return q;
}
} // namespace
struct M2AnimationData {
    uint32_t duration = 0;
    uint32_t blendTime=0;
    std::vector<Bone> bones;
    std::vector<uint32_t> order;
};
bool M2Animation::initialized() const noexcept {return bool(data);}
uint32_t M2Animation::blendTime() const {return data?data->blendTime:0;}
uint32_t M2Animation::duration() const { return data ? data->duration : 0; }
size_t M2Animation::boneCount() const { return data ? data->bones.size() : 0; }
namespace {
std::shared_ptr<const M2AnimationData> decodeData(const std::vector<uint8_t> &b, uint32_t sequenceIndex,
                                                  Metadata &metadata, const ExternalSource &external) {
    const auto header = metadata.header;
    static_assert(sizeof(M2CompBone) == 88 && sizeof(M2Sequence) == 64);
    metadata.target(sequenceIndex); // Validate aliases even for an empty clip.
    const auto &sequence = metadata.sequences[sequenceIndex];
    const auto loops = m2::array<M2Loop>(b, 0x14, 4096, header);
    const auto bones = m2::array<M2CompBone>(b, 0x2c, 4096, header);
    auto out = std::make_shared<M2AnimationData>();
    out->duration = sequence.m_duration;
    out->blendTime=sequence.m_blendTime;
    out->bones.reserve(bones.size());
    Budget budget;
    for (size_t i = 0; i < bones.size(); ++i) {
        const auto &source = bones[i];
        Bone bone;
        bone.parent = source.m_parentIndex;
        bone.id = source.m_boneId;
        bone.flags = source.m_flags;
        bone.nameCRC = source.m_boneNameCRC;
        bone.pivot = source.m_pivot;
        finite(bone.pivot);
        require(bone.parent == 0xffff || (bone.parent < bones.size() && bone.parent != i),
                "Invalid M2 bone parent");
        // Billboard (0x8..0x40) and parent-inheritance (0x1..0x4) bones are evaluated as
        // regular bones: geometry stays correct except camera-facing sprites.
        bone.animated = source.m_flags & 0x280;
        if (bone.animated) {
            size_t at = m2::u32(b, 0x30) + i * sizeof(M2CompBone);
            auto vector = [](const C3Vector &value) {
                finite(value);
                return value;
            };
            bone.translation = track<C3Vector, C3Vector>(b, at + 16, header, sequenceIndex, loops,
                                                         metadata.sequences, external, budget, vector);
            bone.scale = track<C3Vector, C3Vector>(b, at + 56, header, sequenceIndex, loops,
                                                   metadata.sequences, external, budget, vector);
            bone.rotation =
                track<M2CompQuat, C4Quaternion>(b, at + 36, header, sequenceIndex, loops, metadata.sequences,
                                                external, budget, [](const M2CompQuat &raw) {
                                                    C4Quaternion q;
                                                    q.x = raw.m_values[0] * 0.000030518044f - 1.0f;
                                                    q.y = raw.m_values[1] * 0.000030518044f - 1.0f;
                                                    q.z = raw.m_values[2] * 0.000030518044f - 1.0f;
                                                    q.w = raw.m_values[3] * 0.000030518044f - 1.0f;
                                                    valid(q);
                                                    return q;
                                                });
        }
        out->bones.push_back(std::move(bone));
    }
    std::vector<uint8_t> state(bones.size());
    for (uint32_t i = 0; i < bones.size(); ++i) {
        uint32_t current = i;
        std::vector<uint32_t> path;
        while (current != 0xffff && !state[current]) {
            state[current] = 1;
            path.push_back(current);
            current = out->bones[current].parent;
        }
        require(current == 0xffff || state[current] == 2, "Cyclic M2 bone hierarchy");
        while (!path.empty()) {
            auto next = path.back();
            path.pop_back();
            state[next] = 2;
            out->order.push_back(next);
        }
    }
    return out;
}
} // namespace
M2Animation decodeM2Animation(const std::vector<uint8_t> &model, uint32_t sequenceIndex) {
    Metadata metadata(model);
    const ExternalSource missing = [](uint32_t) -> const std::vector<uint8_t> & {
        throw std::invalid_argument("External M2 animation bytes are required");
    };
    return M2Animation(decodeData(model, sequenceIndex, metadata, missing));
}
M2Animation decodeM2Animation(const std::vector<uint8_t> &model, uint32_t sequenceIndex,
                              const std::vector<uint8_t> &animation) {
    require(animation.size() <= 128 * 1024 * 1024, "M2 animation resource exceeds budget");
    Metadata metadata(model);
    const auto resource = metadata.resource(sequenceIndex);
    const ExternalSource source = [&](uint32_t slot) -> const std::vector<uint8_t> & {
        require(metadata.resource(slot) == resource,
                "External fallback needs an additional animation resource");
        return animation;
    };
    return M2Animation(decodeData(model, sequenceIndex, metadata, source));
}
M2Animation loadM2Animation(const std::string &path, uint32_t sequenceIndex) {
    const auto model = m2::readFile(path);
    return loadM2Animations(path, model, {sequenceIndex}).front();
}
std::vector<M2Animation> loadM2Animations(const std::string &path, const std::vector<uint8_t> &model,
                                         const std::vector<uint32_t> &sequenceIndices) {
    require(!sequenceIndices.empty() && sequenceIndices.size() <= 32, "Invalid M2 preload slot count");
    Metadata metadata(model);
    std::vector<uint32_t> sorted = sequenceIndices;
    std::sort(sorted.begin(), sorted.end());
    require(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "Duplicate M2 preload slot");
    for (auto index : sequenceIndices)
        require(index < metadata.sequences.size(), "M2 preload slot exceeds sequence table");
    std::map<std::pair<uint16_t, uint16_t>, std::vector<uint8_t>> resources;
    size_t totalBytes = 0;
    const ExternalSource source = [&](uint32_t slot) -> const std::vector<uint8_t> & {
        const auto key = metadata.resource(slot);
        auto found = resources.find(key);
        if (found != resources.end())
            return found->second;
        constexpr size_t limit = 128 * 1024 * 1024;
        auto bytes = m2::readFile(animationPath(path, key), uint32_t(limit - totalBytes));
        totalBytes += bytes.size();
        return resources.emplace(key, std::move(bytes)).first->second;
    };
    std::vector<M2Animation> clips;
    clips.reserve(sequenceIndices.size());
    for (auto index : sequenceIndices)
        clips.push_back(M2Animation(decodeData(model, index, metadata, source)));
    return clips;
}
namespace {
uint32_t clockTime(const M2AnimationData &clip, M2AnimationClock clock) {
    return clip.duration ? (clock.loop ? uint32_t(clock.milliseconds % clip.duration)
                                       : uint32_t(std::min<uint64_t>(clock.milliseconds, clip.duration)))
                         : 0;
}
template <class T> bool sameClock(const Track<T> &a, const Track<T> &b) {
    return a.step == b.step && a.global == b.global && a.period == b.period && a.loopIndex == b.loopIndex &&
           a.sequenceCount == b.sequenceCount;
}
void compatible(const M2AnimationData &primary, const M2AnimationData &secondary) {
    require(primary.bones.size() == secondary.bones.size(), "M2 blend bone count mismatch");
    for (size_t i = 0; i < primary.bones.size(); ++i) {
        const auto &a = primary.bones[i];
        const auto &b = secondary.bones[i];
        require(a.parent == b.parent && a.animated == b.animated && a.id == b.id && a.flags == b.flags &&
                    a.nameCRC == b.nameCRC && a.pivot.x == b.pivot.x && a.pivot.y == b.pivot.y &&
                    a.pivot.z == b.pivot.z,
                "Incompatible M2 blend skeleton");
        require(sameClock(a.translation, b.translation) && sameClock(a.scale, b.scale) &&
                    sameClock(a.rotation, b.rotation),
                "Incompatible M2 blend track metadata");
    }
}
void validWeight(float weight) {
    require(std::isfinite(weight) && weight >= 0 && weight <= 1, "Invalid M2 blend weight");
}
C4Quaternion crossRotation(float weight, const C4Quaternion &a, const C4Quaternion &b) {
    const auto q = C4Quaternion::Slerp(weight, a, b);
    valid(q);
    return q;
}
// M2AnimateTrack returns early for step tracks, and global-loop channels never
// read the secondary sequence. Missing linear channels still blend defaults.
template <class T, class Interpolate, class Mix>
T channel(const Track<T> &primary, const Track<T> *secondary, uint32_t primaryTime, uint32_t secondaryTime,
          uint64_t globalTime, const T &fallback, float weight, Interpolate interpolate, Mix mix) {
    const auto value = sample(primary, primaryTime, globalTime, fallback, interpolate);
    if (!secondary || weight == 0 || primary.step || primary.global)
        return value;
    return mix(weight, value, sample(*secondary, secondaryTime, globalTime, fallback, interpolate));
}
std::vector<C44Matrix> pose(const M2AnimationData &primary, const M2AnimationData *secondary,
                            M2AnimationClock primaryClock, M2AnimationClock secondaryClock,
                            uint64_t globalTime, float uniformWeight, const std::vector<float> *weights,
                            const C44Matrix &world) {
    finite(world);
    require(world.a3 == 0 && world.b3 == 0 && world.c3 == 0 && world.d3 == 1,
            "M2 pose needs affine world matrix");
    const uint32_t primaryTime = clockTime(primary, primaryClock);
    const uint32_t secondaryTime = secondary ? clockTime(*secondary, secondaryClock) : 0;
    std::vector<C44Matrix> result(primary.bones.size());
    C4Quaternion identity{};
    identity.w = 1;
    for (uint32_t index : primary.order) {
        const auto &bone = primary.bones[index];
        const auto *other = secondary ? &secondary->bones[index] : nullptr;
        const auto &parent = bone.parent == 0xffff ? world : result[bone.parent];
        if (!bone.animated) {
            result[index] = parent;
            continue;
        }
        const float weight = weights ? (*weights)[index] : uniformWeight;
        const auto translation = channel(bone.translation, other ? &other->translation : nullptr, primaryTime,
                                         secondaryTime, globalTime, C3Vector(0, 0, 0), weight, lerp, lerp);
        const auto scale = channel(bone.scale, other ? &other->scale : nullptr, primaryTime, secondaryTime,
                                   globalTime, C3Vector(1, 1, 1), weight, lerp, lerp);
        const auto rotation = channel(bone.rotation, other ? &other->rotation : nullptr, primaryTime,
                                      secondaryTime, globalTime, identity, weight, qlerp, crossRotation);
        C44Matrix local(rotation);
        local.Scale(scale);
        local.d0 = bone.pivot.x + translation.x;
        local.d1 = bone.pivot.y + translation.y;
        local.d2 = bone.pivot.z + translation.z;
        local.Translate(C3Vector(-bone.pivot.x, -bone.pivot.y, -bone.pivot.z));
        result[index] = local * parent;
        finite(result[index]);
    }
    return result;
}
} // namespace
std::vector<C44Matrix> M2Animation::evaluate(uint64_t sequenceTime, uint64_t globalTime, bool loop,
                                             const C44Matrix &world) const {
    if (!data)
        throw std::logic_error("M2 animation is not initialized");
    return pose(*data, nullptr, {sequenceTime, loop}, {}, globalTime, 0, nullptr, world);
}
std::vector<C44Matrix> M2Animation::evaluateBlended(const M2Animation &secondary,
                                                    M2AnimationClock primaryTime,
                                                    M2AnimationClock secondaryTime, uint64_t globalTime,
                                                    float secondaryWeight, const C44Matrix &world) const {
    if (!data)
        throw std::logic_error("M2 animation is not initialized");
    validWeight(secondaryWeight);
    if (secondaryWeight == 0)
        return evaluate(primaryTime.milliseconds, globalTime, primaryTime.loop, world);
    if (!secondary.data)
        throw std::logic_error("Secondary M2 animation is not initialized");
    compatible(*data, *secondary.data);
    return pose(*data, secondary.data.get(), primaryTime, secondaryTime, globalTime, secondaryWeight, nullptr,
                world);
}
std::vector<C44Matrix> M2Animation::evaluateBlended(const M2Animation &secondary,
                                                    M2AnimationClock primaryTime,
                                                    M2AnimationClock secondaryTime, uint64_t globalTime,
                                                    const std::vector<float> &secondaryWeights,
                                                    const C44Matrix &world) const {
    if (!data)
        throw std::logic_error("M2 animation is not initialized");
    require(secondaryWeights.size() == data->bones.size(), "M2 blend weight count mismatch");
    bool any = false;
    for (float weight : secondaryWeights) {
        validWeight(weight);
        any = any || weight != 0;
    }
    if (!any)
        return evaluate(primaryTime.milliseconds, globalTime, primaryTime.loop, world);
    if (!secondary.data)
        throw std::logic_error("Secondary M2 animation is not initialized");
    compatible(*data, *secondary.data);
    return pose(*data, secondary.data.get(), primaryTime, secondaryTime, globalTime, 0, &secondaryWeights,
                world);
}
} // namespace wow::android
