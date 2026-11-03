#include "GlueScene.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace wow::android {
namespace {
uint32_t u32(const std::vector<uint8_t> &b, size_t o) {
    if (o > b.size() || b.size() - o < 4)
        throw std::invalid_argument("Truncated M2 scene field");
    return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 |
           uint32_t(b[o + 3]) << 24;
}
float f32(const std::vector<uint8_t> &b, size_t o) {
    const uint32_t bits = u32(b, o);
    float v;
    std::memcpy(&v, &bits, 4);
    return std::isfinite(v) ? v : 0.f;
}
Vec3 vec(const std::vector<uint8_t> &b, size_t o) { return {f32(b, o), f32(b, o + 4), f32(b, o + 8)}; }
bool inside(const std::vector<uint8_t> &b, uint64_t offset, uint64_t bytes) {
    return offset <= b.size() && bytes <= b.size() - offset;
}
// M2Track<M2SplineKey<C3Vector>> for sequence 0: times[0], keys[0] (36-byte keys).
M2VecTrack track(const std::vector<uint8_t> &b, size_t at, size_t keySize) {
    M2VecTrack t;
    const uint32_t timeCount = u32(b, at + 4), timeOffset = u32(b, at + 8);
    const uint32_t keyCount = u32(b, at + 12), keyOffset = u32(b, at + 16);
    if (!timeCount || !keyCount || !inside(b, timeOffset, 8) || !inside(b, keyOffset, 8))
        return t;
    const uint32_t times = u32(b, timeOffset), timesAt = u32(b, timeOffset + 4);
    const uint32_t keys = u32(b, keyOffset), keysAt = u32(b, keyOffset + 4);
    if (!times || times != keys || times > 4096 || !inside(b, timesAt, uint64_t(times) * 4) ||
        !inside(b, keysAt, uint64_t(keys) * keySize))
        return t; // external .anim keys or malformed: use base position
    for (uint32_t i = 0; i < times; ++i) {
        t.times.push_back(u32(b, timesAt + i * 4));
        t.values.push_back(vec(b, keysAt + i * keySize));
    }
    return t;
}
} // namespace
TexturePixels composeCharacterTexture(
    const TexturePixels &base, const std::vector<std::pair<CharacterSection, TexturePixels>> &layers) {
    if (!base.width || !base.height || base.rgba.size() != size_t(base.width) * base.height * 4)
        throw std::invalid_argument("Invalid base skin texture");
    TexturePixels out = base;
    for (const auto &[section, layer] : layers) {
        if (!layer.width || !layer.height || layer.rgba.size() != size_t(layer.width) * layer.height * 4)
            continue;
        const auto r = characterSectionRect(section, out.width, out.height);
        for (uint32_t y = 0; y < r[3]; ++y)
            for (uint32_t x = 0; x < r[2]; ++x) {
                const uint32_t sx = x * layer.width / r[2], sy = y * layer.height / r[3];
                const uint8_t *s = &layer.rgba[(size_t(sy) * layer.width + sx) * 4];
                uint8_t *d = &out.rgba[(size_t(r[1] + y) * out.width + r[0] + x) * 4];
                const uint32_t a = s[3];
                if (!a)
                    continue;
                for (int c = 0; c < 3; ++c)
                    d[c] = uint8_t((s[c] * a + d[c] * (255 - a) + 127) / 255);
                d[3] = std::max(d[3], s[3]);
            }
    }
    return out;
}
Vec3 M2VecTrack::at(uint32_t ms, const Vec3 &base) const {
    if (times.empty())
        return base;
    Vec3 v = values.front();
    if (times.size() > 1) {
        const uint32_t end = std::max(times.back(), 1u);
        ms %= end;
        size_t i = 0;
        while (i + 1 < times.size() && times[i + 1] <= ms)
            ++i;
        if (i + 1 < times.size() && times[i + 1] > times[i]) {
            const double k = double(ms - std::min(ms, times[i])) / double(times[i + 1] - times[i]);
            for (int c = 0; c < 3; ++c)
                v[c] = values[i][c] + (values[i + 1][c] - values[i][c]) * std::clamp(k, 0.0, 1.0);
        } else
            v = values[i];
    }
    return {base[0] + v[0], base[1] + v[1], base[2] + v[2]};
}
std::optional<Vec3> M2SceneInfo::attachment(uint32_t id) const {
    for (const auto &[a, p] : attachments)
        if (a == id)
            return p;
    return std::nullopt;
}
M2SceneInfo decodeM2Scene(const std::vector<uint8_t> &b) {
    if (b.size() < 0x130 || u32(b, 0) != 0x3032444d)
        throw std::invalid_argument("Invalid scene M2 header");
    M2SceneInfo info;
    const uint32_t sequences = u32(b, 0x1c), sequencesAt = u32(b, 0x20);
    if (sequences && inside(b, sequencesAt, 64))
        info.duration = u32(b, sequencesAt + 4);
    const uint32_t attachments = u32(b, 0xf0), attachmentsAt = u32(b, 0xf4);
    if (attachments <= 256 && inside(b, attachmentsAt, uint64_t(attachments) * 0x28))
        for (uint32_t i = 0; i < attachments; ++i) {
            const size_t a = attachmentsAt + i * 0x28;
            info.attachments.push_back({u32(b, a), vec(b, a + 8)});
        }
    const uint32_t cameras = u32(b, 0x110), camerasAt = u32(b, 0x114);
    if (cameras && inside(b, camerasAt, 0x64)) {
        const size_t c = camerasAt;
        const float fov = f32(b, c + 4), farClip = f32(b, c + 8), nearClip = f32(b, c + 12);
        if (fov > 0 && fov < 3.1415927f && farClip > nearClip && nearClip > 0) {
            info.hasCamera = true;
            info.fov = fov;
            info.farClip = farClip;
            info.nearClip = nearClip;
            info.position = track(b, c + 0x10, 36);
            info.positionBase = vec(b, c + 0x24);
            info.target = track(b, c + 0x30, 36);
            info.targetBase = vec(b, c + 0x44);
        }
    }
    return info;
}
std::optional<M2PortraitCamera> decodeM2PortraitCamera(const std::vector<uint8_t> &b) {
    if (b.size() < 0x130 || u32(b, 0) != 0x3032444d)
        return std::nullopt;
    const uint32_t cameras = u32(b, 0x110), camerasAt = u32(b, 0x114);
    if (!cameras || cameras > 64 || !inside(b, camerasAt, uint64_t(cameras) * 0x64))
        return std::nullopt;
    for (uint32_t i = 0; i < cameras; ++i) {
        const size_t c = camerasAt + size_t(i) * 0x64;
        if (u32(b, c) != 0) // type 0: portrait
            continue;
        const float fov = f32(b, c + 4);
        M2PortraitCamera camera;
        camera.fov = fov > 0 && fov < 3.1415927f ? fov : .7854f;
        camera.eye = track(b, c + 0x10, 36).at(0, vec(b, c + 0x24));
        camera.target = track(b, c + 0x30, 36).at(0, vec(b, c + 0x44));
        return camera;
    }
    return std::nullopt;
}
std::optional<std::string> findGlueModelPath(const std::string &text) {
    // Plain scan (no std::regex: its recursion can overflow small worker-thread stacks).
    auto lowerAt = [&](size_t i) { return char(std::tolower(static_cast<unsigned char>(text[i]))); };
    auto startsWith = [&](size_t at, const char *word) {
        for (size_t k = 0; word[k]; ++k)
            if (at + k >= text.size() || lowerAt(at + k) != word[k])
                return false;
        return true;
    };
    std::string best;
    int bestScore = 0;
    for (size_t at = 0; at < text.size(); ++at) {
        if (!startsWith(at, "interface"))
            continue;
        std::string path;
        size_t i = at;
        while (i < text.size()) {
            const char c = text[i];
            if (c == '/' || c == '\\') {
                if (path.empty() || path.back() != '\\')
                    path += '\\';
            } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-')
                path += c;
            else
                break;
            ++i;
        }
        std::string low;
        for (char c : path)
            low += char(std::tolower(static_cast<unsigned char>(c)));
        if (low.rfind("interface\\glues\\models\\", 0) != 0)
            continue;
        const auto dot = low.find_last_of('.');
        if (dot == std::string::npos)
            continue;
        const auto ext = low.substr(dot);
        if (ext != ".m2" && ext != ".mdx")
            continue;
        // GlueXML lists one login model per expansion; the newest (Northrend) wins.
        const int score = low.find("northrend") != std::string::npos ? 3
                          : low.find("burningcrusade") != std::string::npos ? 2 : 1;
        if (score > bestScore) {
            bestScore = score;
            best = path.substr(0, dot) + ".m2";
        }
    }
    if (bestScore)
        return best;
    return std::nullopt;
}
std::optional<std::string> findGlueMusicName(const std::string &text) {
    std::string best;
    for (size_t at = text.find("GS_"); at != std::string::npos; at = text.find("GS_", at + 3)) {
        size_t end = at;
        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_'))
            ++end;
        const auto name = text.substr(at, end - at);
        if (name.size() > 3 && (best.empty() || name.find("LichKing") != std::string::npos))
            best = name;
    }
    if (best.empty())
        return std::nullopt;
    return best;
}
} // namespace wow::android
