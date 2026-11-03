#include "Engine/Source/Gx/CGxVertexPC.h"
#include "Engine/Source/Gx/Gx.h"
#include "WorldLighting.h"
#include "Frustum.h"
#include <cmath>
#include <algorithm>
#include <optional>
#include "Engine/Source/ShaderEffect/Types.h"
#include "M2File.h"
#include "TerrainPreview.h"
#include "TerrainPreviewTextures.h"
#include "GxStaticMesh.h"
namespace wow::android {
void TerrainPreview::draw(float aspect, TerrainPreviewTextures *textures, bool showProbe) const {
    drawWith(view(), projection(aspect), textures, showProbe, followingProbe(), nullptr, 0);
}
void TerrainPreview::drawWith(const C44Matrix &viewMatrix, const C44Matrix &transform,
                              TerrainPreviewTextures *textures, bool showProbe, bool perspective,
                              const double *focus, double range, GxStaticMesh *mesh,
                              double detailRange) const {
    const auto &g = geometry();
    if (textures)
        m2::require(textures->matches(materials_), "Mismatched terrain GPU texture cache");
    if (g.indices.empty())
        return;
    struct ProjectionMode {
        int32_t previous = GxMasterEnable(GxMasterEnable_NormalProjection);
        ~ProjectionMode() { GxMasterEnableSet(GxMasterEnable_NormalProjection, previous); }
    } projectionMode;
    // Perspective must bypass the legacy optional .2 clip-space scaling.
    if (perspective)
        GxMasterEnableSet(GxMasterEnable_NormalProjection, 1);
    GxRsSet(GxRs_Lighting, 0);
    applyWorldFog();
    GxRsSet(GxRs_Texture0, static_cast<void *>(nullptr));
    GxRsSet(GxRs_Texture1, static_cast<void *>(nullptr));
    GxRsSet(GxRs_TextureShader0, 0);
    GxRsSet(GxRs_TextureShader1, 0);
    GxRsSet(GxRs_TexGen0, GxTexGen_Disabled);
    GxRsSet(GxRs_TexGen1, GxTexGen_Disabled);
    GxRsSet(GxRs_ColorOp0, GxTexOp_PassThru);
    GxRsSet(GxRs_ColorOp1, GxTexOp_PassThru);
    GxRsSet(GxRs_AlphaOp0, GxTexOp_PassThru);
    GxRsSet(GxRs_AlphaOp1, GxTexOp_PassThru);
    GxRsSet(GxRs_MatDiffuse, int32_t(worldLit(0xff8fa868u)));
    GxRsSet(GxRs_AlphaRef, 0);
    GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
    GxRsSet(GxRs_DepthFunc, GxCmp_LEqual);
    GxRsSet(GxRs_DepthTest, 1);
    GxRsSet(GxRs_DepthWrite, 1);
    GxRsSet(GxRs_Culling, 0);
    GxXformSetProjection(transform);
    GxXformSetView(viewMatrix);
    GxXformSet(GxXform_World, C44Matrix(1));
    GxXformSet(GxXform_Tex0, C44Matrix(1));
    GxXformSet(GxXform_Tex1, C44Matrix(1));
    GxRsSet(GxRs_TextureCoord0, 0);
    GxRsSet(GxRs_TextureCoord1, 1);
    static_assert(sizeof(TerrainVertex) == 40, "TerrainVertex must match GxVBF_PNT2");
    if (mesh) {
        if (!mesh->ready()) {
            mesh->setVertices(g.vertices.data(), uint32_t(g.vertices.size()), sizeof(TerrainVertex), GxVBF_PNT2);
            mesh->setIndices(g.indices.data(), uint32_t(g.indices.size()));
        }
        mesh->bind();
    } else {
        GxPrimVertexPtr(g.vertices.size(), &g.vertices[0].position, sizeof(TerrainVertex),
                        &g.vertices[0].normal, sizeof(TerrainVertex), nullptr, 0, &g.vertices[0].uv,
                        sizeof(TerrainVertex), &g.vertices[0].maskUv, sizeof(TerrainVertex));
        GxPrimIndexPtr(g.indices.size(), g.indices.data());
    }
    // World view: cull chunks outside the view frustum (bounding sphere around the center vertex;
    // vertical extent measured once per chunk).
    std::optional<Frustum> frustum;
    if (focus && perspective) {
        frustum.emplace(viewMatrix * transform);
        if (chunkRadius_.size() != g.chunks.size()) {
            chunkRadius_.assign(g.chunks.size(), 24.f);
            for (size_t i = 0; i < g.chunks.size(); ++i) {
                const auto &chunk = g.chunks[i];
                const float cz = g.vertices[chunk.firstVertex + 72].position.z;
                float extent = 0;
                for (uint32_t v = 0; v < 145; ++v)
                    extent = std::max(extent, std::fabs(g.vertices[chunk.firstVertex + v].position.z - cz));
                chunkRadius_[i] = std::sqrt(24.f * 24.f + extent * extent);
            }
        }
    }
    for (size_t i = 0; i < g.chunks.size(); ++i) {
        const auto &chunk = g.chunks[i];
        if (!chunk.indexCount)
            continue;
        if (frustum) {
            const auto &c = g.vertices[chunk.firstVertex + 72].position;
            if (!frustum->sphere(c.x, c.y, c.z, chunkRadius_[i]))
                continue;
        }
        size_t maxPasses = 4;
        if (focus) {
            const auto &c = g.vertices[chunk.firstVertex + 72].position;
            const double dx = c.x - focus[0], dy = c.y - focus[1];
            const double d2 = dx * dx + dy * dy;
            if (d2 > (range + 24) * (range + 24))
                continue;
            // Distant chunks: base layer only (blend layers are barely visible there).
            if (detailRange > 0 && d2 > (detailRange + 24) * (detailRange + 24))
                maxPasses = 1;
        }
        const bool textured = textures && materials_ && materials_->chunks[i].texture >= 0;
        CGxBatch batch{GxPrim_Triangles, chunk.firstIndex, chunk.indexCount,
                       uint16_t(chunk.firstVertex), uint16_t(chunk.firstVertex + 144)};
        auto draw = [&](const TerrainPreviewPass *pass, size_t layer, bool additive) {
            const bool mask = pass && pass->mask >= 0;
            GxRsSet(GxRs_Texture0, pass ? textures->get(size_t(pass->texture)) : nullptr);
            GxRsSet(GxRs_Texture1, mask ? textures->get(size_t(pass->mask)) : nullptr);
            GxRsSet(GxRs_ColorOp0, pass ? GxTexOp_Mod : GxTexOp_PassThru);
            GxRsSet(GxRs_ColorOp1, mask && additive ? GxTexOp_Mod : GxTexOp_PassThru);
            GxRsSet(GxRs_AlphaOp0, GxTexOp_PassThru);
            GxRsSet(GxRs_AlphaOp1, mask && !additive ? GxTexOp_Mod : GxTexOp_PassThru);
            GxRsSet(GxRs_MatDiffuse, int32_t(worldLit(pass ? 0xffffffffu : 0xff8fa868u)));
            GxRsSet(GxRs_BlendingMode,
                    layer ? (additive ? GxBlend_NoAlphaAdd : GxBlend_Alpha) : GxBlend_Opaque);
            GxRsSet(GxRs_DepthWrite, layer ? 0 : 1);
            GxDraw(&batch, 1);
        };
        if (textured) {
            const auto &material = materials_->chunks[i];
            for (size_t layer = 0; layer < material.passes.size() && layer < maxPasses; ++layer)
                draw(&material.passes[layer], layer, material.additive);
        } else
            draw(nullptr, 0, false);
    }
    GxRsSet(GxRs_DepthWrite, 1);
    if (showProbe && probeEnabled_ && walker_->placed()) {
        const auto &p = walker_->position();
        CGxVertexPC vertices[5];
        const float offsets[5][3]{
            {-3, -3, .15f}, {3, -3, .15f}, {3, 3, .15f}, {-3, 3, .15f}, {0, 0, 6}};
        for (size_t i = 0; i < 5; ++i) {
            vertices[i].p = {float(p.x + offsets[i][0]), float(p.y + offsets[i][1]),
                             float(p.z + offsets[i][2])};
            vertices[i].c = CImVector(0, 180, 255, 255);
        }
        const uint16_t indices[]{0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4};
        GxRsSet(GxRs_Texture0, static_cast<void *>(nullptr));
        GxRsSet(GxRs_Texture1, static_cast<void *>(nullptr));
        GxRsSet(GxRs_ColorOp0, GxTexOp_PassThru);
        GxRsSet(GxRs_ColorOp1, GxTexOp_PassThru);
        GxRsSet(GxRs_AlphaOp0, GxTexOp_PassThru);
        GxRsSet(GxRs_AlphaOp1, GxTexOp_PassThru);
        GxRsSet(GxRs_BlendingMode, GxBlend_Opaque);
        GxRsSet(GxRs_MatDiffuse, int32_t(worldLit(0xffffffffu)));
        GxPrimVertexPtr(5, &vertices[0].p, sizeof(CGxVertexPC), nullptr, 0, &vertices[0].c,
                        sizeof(CGxVertexPC), nullptr, 0, nullptr, 0);
        GxPrimIndexPtr(12, indices);
        CGxBatch batch{GxPrim_Triangles, 0, 12, 0, 4};
        GxDraw(&batch, 1);
    }
}
} // namespace wow::android
