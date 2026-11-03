#include "Engine/Source/Gx/Gx.h"
#include "WorldLighting.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "M2File.h"
#include "M2ModelPreview.h"
#include "M2PreviewDrawOrder.h"
#include "M2PreviewTextures.h"
#include "GxStaticMesh.h"
#include <cstring>
#include <cmath>
#include <algorithm>
namespace wow::android {
void M2ModelPreview::draw(float aspect, M2PreviewTextures *textures) const {
    drawTransforms(C44Matrix(1), C44Matrix(1), projection(aspect), textures, false);
}
void M2ModelPreview::drawPlaced(const C44Matrix &world, const C44Matrix &view,
                                const C44Matrix &projection, M2PreviewTextures *textures) const {
    drawTransforms(world, view, projection, textures, true);
}
void M2ModelPreview::drawInstances(const std::vector<C44Matrix> &worlds, const C44Matrix &view,
                                   const C44Matrix &projection, M2PreviewTextures *textures) const {
    if (!worlds.empty())
        drawMany(worlds, view, projection, textures, true);
}
void M2ModelPreview::drawCached(GxStaticMesh &cache, const std::vector<C44Matrix> &worlds,
                                const C44Matrix &view, const C44Matrix &projection,
                                M2PreviewTextures *textures, uint64_t nowMs, uint32_t resampleMs) const {
    if (!worlds.empty())
        drawMany(worlds, view, projection, textures, true, &cache, nowMs, resampleMs);
}
void M2ModelPreview::drawTransforms(const C44Matrix &world, const C44Matrix &view,
                                    const C44Matrix &projection, M2PreviewTextures *textures,
                                    bool normalProjection) const {
    drawMany({world}, view, projection, textures, normalProjection);
}
// One CPU skinning sample shared by every instance (map doodads); draw order uses the
// first instance's transform.
void M2ModelPreview::drawMany(const std::vector<C44Matrix> &worlds, const C44Matrix &view,
                              const C44Matrix &projection, M2PreviewTextures *textures,
                              bool normalProjection, GxStaticMesh *cache, uint64_t nowMs,
                              uint32_t resampleMs) const {
    const auto &world = worlds.front();
    const auto transform = world * view * projection;
    for (const auto *matrix : {&world, &view, &projection, &transform}) {
        const float entries[]{matrix->a0, matrix->a1, matrix->a2, matrix->a3,
                              matrix->b0, matrix->b1, matrix->b2, matrix->b3,
                              matrix->c0, matrix->c1, matrix->c2, matrix->c3,
                              matrix->d0, matrix->d1, matrix->d2, matrix->d3};
        for (float value : entries)
            m2::require(std::isfinite(value), "Nonfinite M2 scene transform");
    }
    const auto &g = model_.geometry();
    const auto materials = model_.previewMaterials();
    // GPU cache: skin once for static poses (detected by comparing two samples 500 ms
    // apart), re-skin animated ones at most every resampleMs.
    bool sample = !cache || !cache->ready();
    if (cache && !sample) {
        if (cache->animated < 0)
            sample = nowMs - cache->sampledAt >= 500;
        else if (cache->animated > 0)
            sample = nowMs - cache->sampledAt >= resampleMs;
    }
    std::vector<M2VertexPNT2> vertices;
    if (sample)
        vertices = model_.sample();
    if (textures)
        m2::require(textures->matches(materials), "Mismatched M2 preview texture cache");
    struct ProjectionMode {
        int32_t previous = GxMasterEnable(GxMasterEnable_NormalProjection);
        ~ProjectionMode() { GxMasterEnableSet(GxMasterEnable_NormalProjection, previous); }
    } projectionMode;
    if (normalProjection)
        GxMasterEnableSet(GxMasterEnable_NormalProjection, 1);
    GxRsSet(GxRs_Lighting, 0);
    applyWorldFog();
    GxRsSet(GxRs_Texture0, static_cast<void *>(nullptr));
    GxRsSet(GxRs_Texture1, static_cast<void *>(nullptr));
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_DepthFunc, GxCmp_LEqual);
    GxRsSet(GxRs_TextureShader0, 0);
    GxRsSet(GxRs_TextureShader1, 0);
    GxRsSet(GxRs_TexGen0, GxTexGen_Disabled);
    GxRsSet(GxRs_TexGen1, GxTexGen_Disabled);
    GxRsSet(GxRs_ColorOp0, GxTexOp_Mod);
    GxRsSet(GxRs_ColorOp1, GxTexOp_PassThru);
    GxRsSet(GxRs_AlphaOp1, GxTexOp_PassThru);
    GxXformSetProjection(projection);
    GxXformSetView(view);
    if (cache) {
        if (sample) {
            const size_t bytes = vertices.size() * sizeof(M2VertexPNT2);
            if (cache->animated < 0) {
                if (cache->firstSample.empty()) {
                    cache->firstSample.resize(bytes);
                    std::memcpy(cache->firstSample.data(), vertices.data(), bytes);
                } else {
                    cache->animated = cache->firstSample.size() != bytes ||
                                      std::memcmp(cache->firstSample.data(), vertices.data(), bytes) != 0;
                    cache->firstSample.clear();
                    cache->firstSample.shrink_to_fit();
                }
            }
            cache->setVertices(vertices.data(), uint32_t(vertices.size()), sizeof(M2VertexPNT2), GxVBF_PNT2);
            if (!cache->ready())
                cache->setIndices(g.indices.data(), uint32_t(g.indices.size()));
            cache->sampledAt = nowMs;
            if (materials && !materials->batches.empty())
                cache->order = m2PreviewDrawOrder(*materials, g, vertices, transform, &geosets_->visible);
        }
        cache->bind();
    } else {
        GxPrimVertexPtr(vertices.size(), &vertices[0].m_position, sizeof(M2VertexPNT2),
                        &vertices[0].m_normal, sizeof(M2VertexPNT2), nullptr, 0,
                        &vertices[0].m_texcoord[0], sizeof(M2VertexPNT2), &vertices[0].m_texcoord[1],
                        sizeof(M2VertexPNT2));
        GxPrimIndexPtr(g.indices.size(), g.indices.data());
    }
    const M2EffectsData *effects = effects_ && effects_->animatedMaterials() ? effects_.get() : nullptr;
    const uint32_t sequenceMs = model_.playback().position();
    const uint64_t globalMs = model_.playback().globalTime();
    auto render = [&](const M2SkinSection &section, const M2PreviewBatch *material) {
        float alpha = material ? material->alpha : 1.f;
        float rgb[3]{1, 1, 1};
        if (effects && material) {
            alpha = 1;
            float value = 1;
            if (material->colorIndex < effects->colorAlpha.size() && !effects->colorAlpha[material->colorIndex].empty()) {
                effects->colorAlpha[material->colorIndex].sample(sequenceMs, globalMs, &value);
                alpha *= value;
            }
            if (material->colorIndex < effects->colorRgb.size() && !effects->colorRgb[material->colorIndex].empty())
                effects->colorRgb[material->colorIndex].sample(sequenceMs, globalMs, rgb);
            if (material->weightIndex < effects->weights.size() && !effects->weights[material->weightIndex].empty()) {
                effects->weights[material->weightIndex].sample(sequenceMs, globalMs, &value);
                alpha *= value;
            }
            if (!(material->colorIndex < effects->colorAlpha.size() && !effects->colorAlpha[material->colorIndex].empty()) &&
                !(material->weightIndex < effects->weights.size() && !effects->weights[material->weightIndex].empty()))
                alpha = material->alpha;
        }
        if (!section.m_indexCount || alpha < 0.004f)
            return; // invisible (texture weight or color alpha 0)
        const bool textured = material && material->textured() && textures;
        const bool uvAnimated = textured && effects && material->transformIndex < effects->textureTransforms.size();
        GxRsSet(GxRs_TextureShader0, uvAnimated ? 1 : 0);
        if (uvAnimated)
            GxXformSet(GxXform_Tex0, m2TextureMatrix(*effects, material->transformIndex, sequenceMs, globalMs));
        // CM2Shared's original fixed-function color/alpha combiner tables.
        static const int colorOps[8]{0, 0, 4, 2, 1, 5, 1, 2};
        static const int alphaOps[8]{3, 0, 3, 2, 1, 3, 3, 3};
        static const int blendModes[7]{0, 1, 2, 10, 3, 4, 5};
        GxRsSet(GxRs_BlendingMode, textured ? blendModes[material->blend] : GxBlend_Opaque);
        GxRsSet(GxRs_Texture0, textured ? textures->get(size_t(material->texture)) : nullptr);
        GxRsSet(GxRs_Texture1, textured && material->stages == 2
                                   ? textures->get(size_t(material->texture2))
                                   : nullptr);
        GxRsSet(GxRs_ColorOp0, textured ? colorOps[material->combiner[0]] : GxTexOp_PassThru);
        GxRsSet(GxRs_ColorOp1, textured && material->stages == 2 ? colorOps[material->combiner[1]]
                                                                 : GxTexOp_PassThru);
        GxRsSet(GxRs_AlphaOp1, textured && material->stages == 2 ? alphaOps[material->combiner[1]]
                                                                 : GxTexOp_PassThru);
        GxRsSet(GxRs_TextureCoord1, textured && material->stages == 2 ? material->uv2 : 1);
        uint32_t diffuse = textured ? 0xffffffffu : 0xffd8d8d8u;
        if (textured && effects && material->blend >= 2) {
            auto byte = [](float v) { return uint32_t(std::clamp(v, 0.f, 1.f) * 255.f + .5f); };
            diffuse = byte(alpha) << 24 | byte(rgb[0]) << 16 | byte(rgb[1]) << 8 | byte(rgb[2]);
        }
        GxRsSet(GxRs_MatDiffuse, int32_t(worldLit(diffuse)));
        GxRsSet(GxRs_TextureCoord0, textured ? material->uv : 0);
        GxRsSet(GxRs_AlphaOp0, textured ? alphaOps[material->combiner[0]] : GxTexOp_PassThru);
        // Original alpha-key reference for element alpha=1 is 224/255.
        GxRsSet(GxRs_AlphaRef, textured ? (material->blend == 1  ? 224
                                           : material->blend > 1 ? 1
                                                                 : 0)
                                        : 0);
        GxRsSet(GxRs_Culling, textured ? !(material->materialFlags & 4) : 0);
        GxRsSet(GxRs_DepthTest, textured ? !(material->materialFlags & 8) : 1);
        GxRsSet(GxRs_DepthWrite, textured ? !(material->materialFlags & 16) : 1);
        CGxBatch batch{GxPrim_Triangles, section.m_indexStart, section.m_indexCount,
                       section.m_vertexStart,
                       uint16_t(section.m_vertexStart + section.m_vertexCount - 1)};
        GxDraw(&batch, 1);
    };
    std::vector<size_t> computed;
    if (!cache && materials && !materials->batches.empty())
        computed = m2PreviewDrawOrder(*materials, g, vertices, transform, &geosets_->visible);
    const auto &order = cache ? cache->order : computed;
    for (const auto &instance : worlds) {
        GxXformSet(GxXform_World, instance);
        if (materials && !materials->batches.empty())
            for (auto index : order) {
                const auto &batch = materials->batches[index];
                render(g.sections[batch.section], &batch);
            }
        else
            for (size_t i = 0; i < g.sections.size(); ++i)
                if (geosets_->visible[i])
                    render(g.sections[i], nullptr);
    }
    GxRsSet(GxRs_AlphaRef, 0);
    GxRsSet(GxRs_TextureShader0, 0);
}
} // namespace wow::android
