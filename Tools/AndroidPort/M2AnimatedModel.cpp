#include <cstring>
#include <cmath>
#include <optional>
#include "M2AnimatedModel.h"
#include "Engine/Source/Model2/M2Skinning.h"
#include "M2File.h"
#include <algorithm>
#include <map>
namespace wow::android {
struct M2AnimatedModelData {
    M2Geometry geometry;
    std::shared_ptr<const M2PreviewMaterialSource> materialSource;
    std::map<uint32_t, M2Animation> clips;
    std::vector<M2ModelSequence> sequences;
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::map<uint32_t, std::pair<uint16_t, C3Vector>> attachments; // id -> bone, position
};
M2AnimatedModel M2AnimatedModel::load(const std::string &modelPath, const std::string &skinPath,
                                      const std::vector<uint32_t> &sequenceIndices,
                                      bool preloadPreviewMaterials,
                                      const M2TextureReplacements &replacements) {
    m2::require(preloadPreviewMaterials || replacements.empty(),
                "Texture replacements require material preload");
    // Validate the request before reading any assets.
    m2::require(!sequenceIndices.empty() && sequenceIndices.size() <= 32,
                "Invalid M2 preload slot count");
    auto sorted = sequenceIndices;
    std::sort(sorted.begin(), sorted.end());
    m2::require(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(),
                "Duplicate M2 preload slot");
    const auto model = m2::readFile(modelPath);
    const auto skin = m2::readFile(skinPath, 64 * 1024 * 1024);
    auto data = std::make_shared<M2AnimatedModelData>();
    data->geometry = decodeM2Geometry(model, skin);
    std::shared_ptr<const M2PreviewMaterials> materials;
    if (preloadPreviewMaterials) {
        data->materialSource =
            std::make_shared<M2PreviewMaterialSource>(decodeM2PreviewMaterialSource(model, skin));
        materials = std::make_shared<M2PreviewMaterials>(
            prepareM2PreviewMaterials(data->materialSource, replacements));
    }
    // Build the union of section vertex ranges. Unused lookup vertices may have
    // unprepared palette indices; they must never reach the skinning function.
    for (const auto &section : data->geometry.sections)
        if (section.m_vertexCount)
            data->ranges.emplace_back(section.m_vertexStart,
                                      section.m_vertexStart + section.m_vertexCount);
    std::sort(data->ranges.begin(), data->ranges.end());
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (const auto &range : data->ranges) {
        if (merged.empty() || range.first > merged.back().second)
            merged.push_back(range);
        else
            merged.back().second = std::max(merged.back().second, range.second);
    }
    data->ranges = std::move(merged);
    // M2Attachment (0x28 bytes): id, bone, unknown, position, animateAttached track.
    {
        const size_t header = m2::modelHeader(model);
        const uint32_t count = m2::u32(model, 0xf0), offset = m2::u32(model, 0xf4);
        if (count <= 256 && offset >= header && offset <= model.size() &&
            count <= (model.size() - offset) / 0x28)
            for (uint32_t i = 0; i < count; ++i) {
                const size_t at = offset + size_t(i) * 0x28;
                const uint16_t bone = m2::u16(model, at + 4);
                if (bone >= data->geometry.boneCount)
                    continue;
                C3Vector position;
                std::memcpy(&position, model.data() + at + 8, sizeof(position));
                if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
                    continue;
                data->attachments.emplace(m2::u32(model, at), std::make_pair(bone, position));
            }
    }
    auto clips = loadM2Animations(modelPath, model, sequenceIndices);
    const auto sequences = m2::array<M2Sequence>(model, 0x1c, 4096, m2::modelHeader(model));
    for (size_t i = 0; i < clips.size(); ++i) {
        m2::require(clips[i].boneCount() == data->geometry.boneCount,
                    "M2 model pose/geometry mismatch");
        data->clips.emplace(sequenceIndices[i], std::move(clips[i]));
        const auto &sequence = sequences[sequenceIndices[i]];
        data->sequences.push_back(
            {sequenceIndices[i], sequence.m_duration, sequence.m_id, sequence.m_variationIndex});
    }
    return M2AnimatedModel(std::move(data), sequenceIndices.front(), std::move(materials));
}
M2AnimatedModel::M2AnimatedModel(std::shared_ptr<const M2AnimatedModelData> data, uint32_t first,
                                 std::shared_ptr<const M2PreviewMaterials> materials)
    : data_(std::move(data)), materials_(std::move(materials)) {
    play(first);
}
std::shared_ptr<const M2PreviewMaterials> M2AnimatedModel::previewMaterials() const noexcept {
    return materials_;
}
std::shared_ptr<const M2PreviewMaterialSource> M2AnimatedModel::materialSource() const noexcept {
    return data_->materialSource;
}
std::shared_ptr<const M2PreviewMaterials>
M2AnimatedModel::preparePreviewMaterials(const M2TextureReplacements &replacements) const {
    m2::require(bool(data_->materialSource), "Model has no material recipe");
    return std::make_shared<M2PreviewMaterials>(
        prepareM2PreviewMaterials(data_->materialSource, replacements));
}
void M2AnimatedModel::installPreviewMaterials(std::shared_ptr<const M2PreviewMaterials> materials) {
    m2::require(materials && data_->materialSource && materials->source == data_->materialSource,
                "Foreign or missing model appearance");
    materials_ = std::move(materials);
}
const M2Geometry &M2AnimatedModel::geometry() const noexcept { return data_->geometry; }
const std::vector<M2ModelSequence> &M2AnimatedModel::sequences() const noexcept {
    return data_->sequences;
}
void M2AnimatedModel::playAnimation(uint16_t id, uint16_t variation,
                                    const M2PlaybackOptions &options) {
    const M2ModelSequence *selected = nullptr;
    for (const auto &sequence : data_->sequences)
        if (sequence.id == id && sequence.variation == variation) {
            m2::require(!selected, "Ambiguous preloaded M2 animation ID/variation");
            selected = &sequence;
        }
    m2::require(selected, "M2 animation ID/variation was not preloaded");
    play(selected->index, options);
}
void M2AnimatedModel::play(uint32_t sequenceIndex, const M2PlaybackOptions &options) {
    const auto found = data_->clips.find(sequenceIndex);
    m2::require(found != data_->clips.end(), "M2 sequence was not preloaded");
    player_.play(found->second, options);
    sequence_ = sequenceIndex;
}
std::optional<C44Matrix> M2AnimatedModel::attachment(uint32_t id, const C44Matrix &world) const {
    const auto found = data_->attachments.find(id);
    if (found == data_->attachments.end())
        return std::nullopt;
    const auto bones = player_.evaluate(world);
    if (found->second.first >= bones.size())
        return std::nullopt;
    C44Matrix local(1);
    local.d0 = found->second.second.x;
    local.d1 = found->second.second.y;
    local.d2 = found->second.second.z;
    return local * bones[found->second.first];
}
bool M2AnimatedModel::hasAttachment(uint32_t id) const noexcept {
    return data_->attachments.count(id) != 0;
}
std::vector<M2VertexPNT2> M2AnimatedModel::sample(const C44Matrix &world) const {
    const auto bones = player_.evaluate(world);
    const auto &source = data_->geometry.vertices;
    std::vector<M2VertexPNT2> result(source.size());
    if (data_->geometry.wideBones) {
        // More than 256 bones: blend by the global indices (M2BlendBoneMatrices with 16-bit ids).
        const auto &ids = data_->geometry.boneIndices;
        for (const auto &range : data_->ranges)
            for (uint32_t i = range.first; i < range.second; ++i) {
                const auto &v = source[i];
                float m[12]{};
                for (unsigned k = 0; k < 4 && v.m_weights.b[k]; ++k) {
                    const uint16_t id = ids[i][k];
                    if (id >= bones.size())
                        continue;
                    const auto &b = bones[id];
                    const float w = v.m_weights.b[k] / 255.f;
                    const float values[12]{b.a0, b.a1, b.a2, b.b0, b.b1, b.b2, b.c0, b.c1, b.c2, b.d0, b.d1, b.d2};
                    for (int j = 0; j < 12; ++j)
                        m[j] += values[j] * w;
                }
                auto &out = result[i];
                const auto &p = v.m_position, &n = v.m_normal;
                out.m_position = {p.x * m[0] + p.y * m[3] + p.z * m[6] + m[9], p.x * m[1] + p.y * m[4] + p.z * m[7] + m[10],
                                  p.x * m[2] + p.y * m[5] + p.z * m[8] + m[11]};
                out.m_normal = {n.x * m[0] + n.y * m[3] + n.z * m[6], n.x * m[1] + n.y * m[4] + n.z * m[7],
                                n.x * m[2] + n.y * m[5] + n.z * m[8]};
                out.m_texcoord[0] = v.m_texcoord[0];
                out.m_texcoord[1] = v.m_texcoord[1];
            }
        return result;
    }
    for (const auto &range : data_->ranges) {
        const auto vertices = M2Skinning::Transform(
            source.data() + range.first, range.second - range.first, bones.data(), bones.size());
        std::copy(vertices.begin(), vertices.end(), result.begin() + range.first);
    }
    return result;
}
} // namespace wow::android
