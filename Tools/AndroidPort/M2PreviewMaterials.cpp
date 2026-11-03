#include "M2PreviewMaterials.h"
#include "Engine/Source/Model2/M2Data.h"
#include "M2File.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
namespace wow::android {
namespace {
struct RawTexture {
    uint32_t type, flags, count, offset;
};
struct RawMaterial {
    uint16_t flags, blend;
};
static_assert(sizeof(RawTexture) == 16 && sizeof(RawMaterial) == 4 && sizeof(M2Batch) == 24);
uint16_t lookup(const std::vector<uint16_t> &table, uint32_t index) {
    m2::require(index < table.size(), "M2 material lookup exceeds table");
    return table[index];
}
std::string filename(const std::vector<uint8_t> &model, const RawTexture &t, size_t header) {
    auto chars = m2::records<char>(model, t.count, t.offset, 4096, header);
    m2::require(chars.size() > 1 && chars.back() == 0, "Invalid named M2 texture path");
    m2::require(std::find(chars.begin(), chars.end() - 1, 0) == chars.end() - 1,
                "Embedded NUL in M2 texture path");
    return {chars.data(), chars.size() - 1};
}
std::string key(std::string path) {
    for (char &c : path) {
        if (c == '/')
            c = '\\';
        c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return path;
}
} // namespace
size_t M2PreviewMaterials::texturedBatches() const noexcept {
    return std::count_if(batches.begin(), batches.end(),
                         [](const auto &b) { return b.textured(); });
}
M2PreviewMaterialSource decodeM2PreviewMaterialSource(const std::vector<uint8_t> &model,
                                                      const std::vector<uint8_t> &skin) {
    const auto header = m2::modelHeader(model);
    m2::require(skin.size() >= 48 && skin.size() <= 64 * 1024 * 1024 &&
                    m2::u32(skin, 0) == 0x4e494b53,
                "Invalid material SKIN file");
    const auto batches = m2::array<M2Batch>(skin, 36, 65536, 48);
    M2PreviewMaterialSource result;
    if (batches.empty())
        return result;
    const auto sections = m2::array<M2SkinSection>(skin, 28, 65536, 48);
    const auto materials = m2::array<RawMaterial>(model, 0x70, 65536, header);
    const auto textures = m2::array<RawTexture>(model, 0x50, 65536, header);
    const auto combo = m2::array<uint16_t>(model, 0x80, 65536, header);
    const auto coords = m2::array<uint16_t>(model, 0x88, 65536, header);
    const auto weights = m2::array<uint16_t>(model, 0x90, 65536, header);
    const auto transforms = m2::array<uint16_t>(model, 0x98, 65536, header);
    const auto colorCount = m2::u32(model, 0x48), weightCount = m2::u32(model, 0x58),
               transformCount = m2::u32(model, 0x60);
    // Static value of an M2Track<fixed16>: first key of the first non-empty sequence.
    auto staticAlpha = [&](size_t track) -> float {
        if (track + 0x14 > model.size())
            return 1;
        const uint32_t count = m2::u32(model, track + 0xc), at = m2::u32(model, track + 0x10);
        for (uint32_t i = 0; i < count && i < 4096; ++i) {
            const size_t entry = size_t(at) + i * 8;
            if (entry + 8 > model.size())
                return 1;
            const uint32_t keys = m2::u32(model, entry), keysAt = m2::u32(model, entry + 4);
            if (!keys)
                continue;
            if (size_t(keysAt) + 2 > model.size())
                return 1;
            const int16_t v = int16_t(model[keysAt] | model[keysAt + 1] << 8);
            return std::clamp(float(v) / 32767.f, 0.f, 1.f);
        }
        return 1;
    };
    std::vector<float> weightAlpha(std::min<uint32_t>(weightCount, 65536), 1.f),
        colorAlpha(std::min<uint32_t>(colorCount, 65536), 1.f);
    for (size_t i = 0; i < weightAlpha.size(); ++i)
        weightAlpha[i] = staticAlpha(m2::u32(model, 0x5c) + i * 0x14);
    for (size_t i = 0; i < colorAlpha.size(); ++i)
        colorAlpha[i] = staticAlpha(m2::u32(model, 0x4c) + i * 0x28 + 0x14);
    std::map<uint16_t, uint16_t> textureRecords;
    uint64_t textureWork = 0, indexWork = 0;
    const bool customCombiners = (m2::u32(model, 16) & 8) != 0;
    const auto combiners = customCombiners ? m2::array<uint16_t>(model, 0x130, 65536, header)
                                           : std::vector<uint16_t>{};
    auto textureSource = [&](uint16_t index) -> uint16_t {
        const auto found = textureRecords.find(index);
        if (found != textureRecords.end())
            return found->second;
        m2::require(textureRecords.size() < 256, "M2 preview texture count budget exceeded");
        const auto &texture = textures[index];
        const auto slot = uint16_t(result.textures.size());
        result.textures.push_back(
            {texture.type, texture.flags & 3u,
             texture.type ? std::string{} : filename(model, texture, header)});
        textureRecords.emplace(index, slot);
        return slot;
    };
    for (const auto &batch : batches) {
        m2::require(batch.m_skinSectionIndex < sections.size() &&
                        batch.m_materialIndex < materials.size(),
                    "Invalid M2 batch section/material");
        textureWork += batch.m_textureCount;
        indexWork += sections[batch.m_skinSectionIndex].m_indexCount;
        m2::require(textureWork <= 1024 * 1024 && indexWork <= 16 * 1024 * 1024,
                    "M2 preview batch work budget exceeded");
        const auto &material = materials[batch.m_materialIndex];
        m2::require(material.blend < 7, "Invalid M2 material blend mode");
        m2::require(batch.m_colorIndex == 0xffff || batch.m_colorIndex < colorCount,
                    "Invalid M2 batch color index");
        M2PreviewBatch out;
        out.section = batch.m_skinSectionIndex;
        out.materialFlags = material.flags & 31u;
        out.blend = material.blend;
        m2::require(batch.m_textureComboIndex <= combo.size() &&
                        batch.m_textureCount <= combo.size() - batch.m_textureComboIndex,
                    "M2 batch texture window exceeds table");
        for (uint32_t j = 0; j < batch.m_textureCount; ++j)
            m2::require(combo[batch.m_textureComboIndex + j] < textures.size(),
                        "M2 texture combo exceeds textures");
        // Animated color/weight/UV transforms are sampled statically; layers/priority planes
        // keep skin order; special shaders use the default combiner.
        out.colorIndex = batch.m_colorIndex;
        if (batch.m_colorIndex != 0xffff && batch.m_colorIndex < colorAlpha.size())
            out.alpha *= colorAlpha[batch.m_colorIndex];
        if (batch.m_textureCount < 1 || batch.m_textureCount > 2)
            out.fallback = "texture stage count";
        else {
            uint16_t textureIndices[2]{}, uv[2]{};
            for (uint32_t j = 0; j < batch.m_textureCount; ++j) {
                uv[j] = lookup(coords, uint32_t(batch.m_textureCoordComboIndex) + j);
                const auto weight = lookup(weights, uint32_t(batch.m_textureWeightComboIndex) + j);
                const auto transform =
                    lookup(transforms, uint32_t(batch.m_textureTransformComboIndex) + j);
                textureIndices[j] = lookup(combo, uint32_t(batch.m_textureComboIndex) + j);
                const auto &texture = textures[textureIndices[j]];
                uint16_t op = customCombiners && !(batch.m_shader & 0x8000)
                                  ? lookup(combiners, uint32_t(batch.m_shader) + j)
                                  : uint16_t(j == 0 && material.blend != 0);
                if (j == 0 && material.blend == 0)
                    op = 0; // CM2Shared::SubstituteSimpleShaders.
                out.combiner[j] = op;
                if (j == 0) {
                    out.transformIndex = transform;
                    out.weightIndex = weight;
                }
                (void)texture;
                if (uv[j] > 1)
                    uv[j] = 0; // environment-mapped coordinates: use UV0
                if (j == 0 && weight < weightAlpha.size())
                    out.alpha *= weightAlpha[weight];
                if (op > 7)
                    out.combiner[j] = 1;
            }
            if (out.fallback.empty()) {
                M2PreviewBatchSource recipe;
                out.stages = batch.m_textureCount;
                out.uv = uv[0];
                out.uv2 = uv[1];
                recipe.pass = std::move(out);
                for (uint32_t j = 0; j < batch.m_textureCount; ++j)
                    recipe.textures[j] = textureSource(textureIndices[j]);
                result.batches.push_back(std::move(recipe));
                continue;
            }
        }
        result.batches.push_back({std::move(out), {0, 0}});
    }
    return result;
}
namespace {
void validateBindings(const M2TextureReplacements &bindings) {
    m2::require(bindings.size() <= 32, "Too many M2 texture replacement types");
    size_t bytes = 0;
    for (const auto &[type, path] : bindings) {
        (void)type;
        m2::require(!path.empty() && path.size() <= 4096 && path.find('\0') == std::string::npos,
                    "Invalid M2 texture replacement path");
        bytes += path.size();
        m2::require(bytes <= 65536, "M2 texture replacement paths exceed budget");
    }
}
} // namespace
M2TextureReplacements parseM2TextureReplacements(const std::string &text) {
    m2::require(text.size() <= 65536 && text.find('\0') == std::string::npos,
                "Invalid M2 replacement text");
    const auto trim = [](std::string s) {
        auto whitespace = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!s.empty() && whitespace(s.back()))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && whitespace(s[i]))
            ++i;
        return s.substr(i);
    };
    M2TextureReplacements result;
    size_t at = 0;
    while (at < text.size()) {
        auto end = text.find(';', at);
        if (end == std::string::npos)
            end = text.size();
        auto entry = trim(text.substr(at, end - at));
        at = end + 1;
        if (entry.empty())
            continue;
        const auto equal = entry.find('=');
        m2::require(equal != std::string::npos, "Missing replacement type separator");
        auto number = trim(entry.substr(0, equal)), path = trim(entry.substr(equal + 1));
        uint32_t type = 0;
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), type);
        m2::require(!number.empty() && parsed.ec == std::errc{} &&
                        parsed.ptr == number.data() + number.size(),
                    "Invalid replacement type");
        m2::require(result.emplace(type, std::move(path)).second, "Duplicate replacement type");
    }
    validateBindings(result);
    return result;
}
M2PreviewMaterials prepareM2PreviewMaterials(std::shared_ptr<const M2PreviewMaterialSource> source,
                                             const M2TextureReplacements &replacements) {
    validateBindings(replacements);
    m2::require(bool(source), "Missing M2 material recipe");
    m2::require(source->textures.size() <= 256 && source->batches.size() <= 65536,
                "M2 recipe exceeds budget");
    M2PreviewMaterials result;
    result.source = source;
    std::map<uint16_t, int32_t> records;
    std::map<std::string, std::shared_ptr<const TexturePixels>> cache;
    size_t encodedBytes = 0, decodedBytes = 0, textureBytes = 0;
    constexpr size_t budget = 128 * 1024 * 1024;
    auto resolve = [&](uint16_t index, const std::string &path) -> int32_t {
        const auto existing = records.find(index);
        if (existing != records.end())
            return existing->second;
        const auto &texture = source->textures[index];
        const auto normalized = key(path);
        auto found = cache.find(normalized);
        if (found == cache.end()) {
            auto bytes = m2::readFile(
                path, uint32_t(std::min<size_t>(64 * 1024 * 1024, budget - encodedBytes)));
            encodedBytes += bytes.size();
            m2::require(bytes.size() >= 148, "Truncated M2 BLP texture");
            const uint64_t pixels = uint64_t(m2::u32(bytes, 12)) * m2::u32(bytes, 16);
            m2::require(pixels <= (budget - decodedBytes) / 4,
                        "M2 preview decoded texture budget exceeded");
            auto image = std::make_shared<TexturePixels>(decodeBlp2(bytes));
            decodedBytes += image->rgba.size();
            found = cache.emplace(normalized, std::move(image)).first;
        }
        const auto power = [](uint32_t x) { return x && !(x & (x - 1)); };
        if ((texture.flags & 3) &&
            (!power(found->second->width) || !power(found->second->height))) {
            records.emplace(index, -1);
            return -1;
        }
        m2::require(found->second->rgba.size() <= budget - textureBytes,
                    "M2 preview texture upload budget exceeded");
        textureBytes += found->second->rgba.size();
        const auto slot = int32_t(result.textures.size());
        result.textures.push_back({path, texture.flags, found->second, texture.type});
        records.emplace(index, slot);
        return slot;
    };
    for (const auto &recipe : source->batches) {
        auto batch = recipe.pass;
        batch.texture = batch.texture2 = -1;
        if (batch.fallback.empty()) {
            m2::require(batch.stages >= 1 && batch.stages <= 2, "Invalid material recipe stages");
            std::string paths[2];
            for (unsigned j = 0; j < batch.stages; ++j) {
                m2::require(recipe.textures[j] < source->textures.size(),
                            "Invalid material recipe texture");
                const auto &texture = source->textures[recipe.textures[j]];
                m2::require(!(texture.flags & ~3u), "Invalid material recipe texture flags");
                const auto binding = replacements.find(texture.type);
                paths[j] = binding != replacements.end() ? binding->second
                           : texture.type == 0           ? texture.path
                                                         : std::string{};
                if (paths[j].empty())
                    batch.fallback = "unbound replaceable texture";
            }
            if (batch.fallback.empty()) {
                int32_t slots[2]{-1, -1};
                for (unsigned j = 0; j < batch.stages; ++j) {
                    slots[j] = resolve(recipe.textures[j], paths[j]);
                    if (slots[j] < 0) {
                        batch.fallback = "NPOT repeat requires optional GLES capability";
                        break;
                    }
                }
                if (batch.fallback.empty()) {
                    batch.texture = slots[0];
                    batch.texture2 = slots[1];
                }
            }
        }
        result.batches.push_back(std::move(batch));
    }
    return result;
}
M2PreviewMaterials loadM2PreviewMaterials(const std::vector<uint8_t> &model,
                                          const std::vector<uint8_t> &skin,
                                          const M2TextureReplacements &replacements) {
    return prepareM2PreviewMaterials(
        std::make_shared<M2PreviewMaterialSource>(decodeM2PreviewMaterialSource(model, skin)),
        replacements);
}
} // namespace wow::android
