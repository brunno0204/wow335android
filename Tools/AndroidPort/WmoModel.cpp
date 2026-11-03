#include "WmoModel.h"
#include "Frustum.h"
#include <algorithm>
#include <cmath>
#include "Engine/Source/Gx/Gx.h"
#include "WorldLighting.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "M2File.h"
#include "M2PreviewTextures.h"
#include "GxStaticMesh.h"
#include "TextureDecoder.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <algorithm>
#include <stdexcept>
namespace wow::android {
namespace {
constexpr uint32_t tag(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 |
           uint32_t(uint8_t(s[3]));
}
struct Chunk {
    uint32_t id;
    size_t data, size;
};
std::vector<Chunk> chunks(const std::vector<uint8_t> &b, size_t begin, size_t end) {
    std::vector<Chunk> out;
    for (size_t at = begin; at + 8 <= end;) {
        const uint32_t id = m2::u32(b, at), size = m2::u32(b, at + 4);
        if (size > end - at - 8)
            break;
        out.push_back({id, at + 8, size});
        at += 8 + size;
    }
    return out;
}
const Chunk *find(const std::vector<Chunk> &list, uint32_t id) {
    for (const auto &c : list)
        if (c.id == id)
            return &c;
    return nullptr;
}
float f32(const std::vector<uint8_t> &b, size_t o) {
    const uint32_t bits = m2::u32(b, o);
    float v;
    std::memcpy(&v, &bits, 4);
    return std::isfinite(v) ? v : 0.f;
}
} // namespace
size_t WmoModel::triangles() const noexcept {
    size_t n = 0;
    for (const auto &g : groups_)
        n += g.indices.size() / 3;
    return n;
}
WmoModel WmoModel::load(const std::string &rootPath) { return load(rootPath, 0); }
WmoModel WmoModel::load(const std::string &rootPath, uint16_t doodadSet) {
    const auto root = m2::readFile(rootPath, 32 * 1024 * 1024);
    const auto top = chunks(root, 0, root.size());
    const auto *header = find(top, tag("MOHD"));
    m2::require(header && header->size >= 8, "WMO root without MOHD");
    const uint32_t groupCount = m2::u32(root, header->data + 4);
    m2::require(groupCount <= 512, "WMO group count exceeds budget");
    const auto *textureNames = find(top, tag("MOTX"));
    const auto *materials = find(top, tag("MOMT"));
    WmoModel model;
    model.materials_ = std::make_shared<M2PreviewMaterials>();
    std::map<uint32_t, int32_t> textureSlot; // MOTX offset -> slot
    struct Material {
        int32_t texture = -1;
        uint32_t blend = 0, flags = 0;
    };
    std::vector<Material> mats;
    if (materials)
        for (size_t i = 0; i + 64 <= materials->size && mats.size() < 4096; i += 64) {
            const size_t m = materials->data + i;
            Material mat;
            mat.flags = m2::u32(root, m);
            mat.blend = m2::u32(root, m + 8);
            const uint32_t nameOffset = m2::u32(root, m + 12);
            if (textureNames && nameOffset < textureNames->size) {
                auto found = textureSlot.find(nameOffset);
                if (found == textureSlot.end()) {
                    size_t at = textureNames->data + nameOffset, end = at;
                    while (end < textureNames->data + textureNames->size && root[end])
                        ++end;
                    const std::string path(root.begin() + long(at), root.begin() + long(end));
                    int32_t slot = -1;
                    if (!path.empty()) {
                        try {
                            auto pixels = std::make_shared<TexturePixels>(
                                decodeBlp2(m2::readFile(path, 16 * 1024 * 1024)));
                            slot = int32_t(model.materials_->textures.size());
                            model.materials_->textures.push_back({path, 3, std::move(pixels), 0});
                        } catch (const std::exception &) {
                        }
                    }
                    found = textureSlot.emplace(nameOffset, slot).first;
                }
                mat.texture = found->second;
            }
            mats.push_back(mat);
        }
    // Doodad sets: MODS (32 bytes), MODN names, MODD (40 bytes).
    if (const auto *sets = find(top, tag("MODS")), *names = find(top, tag("MODN")),
        *defs = find(top, tag("MODD"));
        sets && names && defs) {
        const size_t setCount = sets->size / 32, defCount = defs->size / 40;
        std::vector<bool> wanted(defCount, false);
        for (size_t set = 0; set < setCount; ++set) {
            if (set != 0 && set != doodadSet)
                continue;
            const uint32_t first = m2::u32(root, sets->data + set * 32 + 20),
                           count = m2::u32(root, sets->data + set * 32 + 24);
            for (uint64_t i = first; i < uint64_t(first) + count && i < defCount; ++i)
                wanted[i] = true;
        }
        for (size_t i = 0; i < defCount && model.doodads_.size() < 4096; ++i) {
            if (!wanted[i])
                continue;
            const size_t d = defs->data + i * 40;
            const uint32_t nameOffset = m2::u32(root, d) & 0xffffff;
            if (nameOffset >= names->size)
                continue;
            size_t at = names->data + nameOffset, end = at;
            while (end < names->data + names->size && root[end])
                ++end;
            std::string path(root.begin() + long(at), root.begin() + long(end));
            const auto dot = path.find_last_of('.');
            if (path.empty() || dot == std::string::npos)
                continue;
            path = path.substr(0, dot) + ".m2";
            const float px = f32(root, d + 4), py = f32(root, d + 8), pz = f32(root, d + 12);
            const double qx = f32(root, d + 16), qy = f32(root, d + 20), qz = f32(root, d + 24),
                         qw = f32(root, d + 28);
            const float scale = f32(root, d + 32);
            C44Matrix m(1);
            m.a0 = float(1 - 2 * (qy * qy + qz * qz));
            m.a1 = float(2 * (qx * qy + qz * qw));
            m.a2 = float(2 * (qx * qz - qy * qw));
            m.b0 = float(2 * (qx * qy - qz * qw));
            m.b1 = float(1 - 2 * (qx * qx + qz * qz));
            m.b2 = float(2 * (qy * qz + qx * qw));
            m.c0 = float(2 * (qx * qz + qy * qw));
            m.c1 = float(2 * (qy * qz - qx * qw));
            m.c2 = float(1 - 2 * (qx * qx + qy * qy));
            const float s = scale > 0 && scale < 64 ? scale : 1;
            m.a0 *= s; m.a1 *= s; m.a2 *= s;
            m.b0 *= s; m.b1 *= s; m.b2 *= s;
            m.c0 *= s; m.c1 *= s; m.c2 *= s;
            m.d0 = px;
            m.d1 = py;
            m.d2 = pz;
            model.doodads_.push_back({std::move(path), m});
        }
    }
    float low[3]{1e30f, 1e30f, 1e30f}, high[3]{-1e30f, -1e30f, -1e30f};
    const auto base = rootPath.substr(0, rootPath.size() - 4);
    for (uint32_t g = 0; g < groupCount; ++g) {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "_%03u.wmo", g);
        std::vector<uint8_t> file;
        try {
            file = m2::readFile(base + suffix, 32 * 1024 * 1024);
        } catch (const std::exception &) {
            continue;
        }
        const auto groupTop = chunks(file, 0, file.size());
        const auto *mogp = find(groupTop, tag("MOGP"));
        if (!mogp || mogp->size < 0x44)
            continue;
        const auto sub = chunks(file, mogp->data + 0x44, mogp->data + mogp->size);
        const auto *movt = find(sub, tag("MOVT"));
        const auto *movi = find(sub, tag("MOVI"));
        const auto *monr = find(sub, tag("MONR"));
        const auto *motv = find(sub, tag("MOTV"));
        const auto *moba = find(sub, tag("MOBA"));
        const auto *mopy = find(sub, tag("MOPY"));
        if (!movt || !movi || !moba)
            continue;
        Group group;
        const size_t vertices = movt->size / 12;
        if (vertices > 65536)
            continue;
        group.vertices.resize(vertices);
        for (size_t v = 0; v < vertices; ++v) {
            auto &out = group.vertices[v];
            out.m_position = {f32(file, movt->data + v * 12), f32(file, movt->data + v * 12 + 4),
                              f32(file, movt->data + v * 12 + 8)};
            if (monr && monr->size >= (v + 1) * 12)
                out.m_normal = {f32(file, monr->data + v * 12), f32(file, monr->data + v * 12 + 4),
                                f32(file, monr->data + v * 12 + 8)};
            else
                out.m_normal = {0, 0, 1};
            for (int k = 0; k < 3; ++k) {
                const float c = k == 0 ? out.m_position.x : k == 1 ? out.m_position.y : out.m_position.z;
                low[k] = std::min(low[k], c);
                high[k] = std::max(high[k], c);
            }
            if (motv && motv->size >= (v + 1) * 8)
                out.m_texcoord[0] = {f32(file, motv->data + v * 8), f32(file, motv->data + v * 8 + 4)};
            out.m_texcoord[1] = out.m_texcoord[0];
        }
        group.indices.resize(movi->size / 2);
        for (size_t i = 0; i < group.indices.size(); ++i) {
            group.indices[i] = m2::u16(file, movi->data + i * 2);
            if (group.indices[i] >= vertices)
                group.indices[i] = 0;
        }
        // Collision faces (MOPY flags: 0x04 detail, 0x08 collision, 0x20 render).
        {
            const uint32_t first = uint32_t(model.collision_.vertices.size());
            bool any = false;
            for (size_t t = 0; t + 2 < group.indices.size(); t += 3) {
                const size_t face = t / 3;
                uint8_t flags = 0x20;
                if (mopy && mopy->size >= (face + 1) * 2)
                    flags = file[mopy->data + face * 2];
                const bool render = (flags & 0x20) && !(flags & 0x04);
                if (!(flags & 0x08) && !render)
                    continue;
                any = true;
                for (int k = 0; k < 3; ++k)
                    model.collision_.indices.push_back(first + group.indices[t + k]);
            }
            if (any)
                for (const auto &v : group.vertices)
                    model.collision_.vertices.push_back(v.m_position);
        }
        for (size_t i = 0; i + 24 <= moba->size; i += 24) {
            const size_t at = moba->data + i;
            Batch batch;
            batch.firstIndex = m2::u32(file, at + 12);
            batch.indexCount = m2::u16(file, at + 16);
            batch.minIndex = m2::u16(file, at + 18);
            batch.maxIndex = m2::u16(file, at + 20);
            const uint8_t material = file[at + 23];
            if (uint64_t(batch.firstIndex) + batch.indexCount > group.indices.size() ||
                batch.maxIndex >= vertices || batch.minIndex > batch.maxIndex)
                continue;
            if (material < mats.size()) {
                batch.texture = mats[material].texture;
                batch.blend = mats[material].blend;
                batch.flags = mats[material].flags;
            }
            group.batches.push_back(batch);
        }
        model.groups_.push_back(std::move(group));
    }
    m2::require(!model.groups_.empty(), "WMO has no drawable groups");
    for (int k = 0; k < 3; ++k)
        model.center_[k] = (low[k] + high[k]) * .5f;
    model.radius_ = std::sqrt((high[0] - low[0]) * (high[0] - low[0]) + (high[1] - low[1]) * (high[1] - low[1]) +
                              (high[2] - low[2]) * (high[2] - low[2])) * .5f;
    return model;
}
void WmoModel::draw(const C44Matrix &world, const C44Matrix &view, const C44Matrix &projection,
                    M2PreviewTextures *textures, std::vector<std::unique_ptr<GxStaticMesh>> *meshes) const {
    if (meshes && meshes->size() != groups_.size()) {
        meshes->clear();
        for (size_t i = 0; i < groups_.size(); ++i)
            meshes->push_back(std::make_unique<GxStaticMesh>());
    }
    struct ProjectionMode {
        int32_t previous = GxMasterEnable(GxMasterEnable_NormalProjection);
        ~ProjectionMode() { GxMasterEnableSet(GxMasterEnable_NormalProjection, previous); }
    } projectionMode;
    GxMasterEnableSet(GxMasterEnable_NormalProjection, 1);
    GxRsSet(GxRs_Lighting, 0);
    applyWorldFog();
    GxRsSet(GxRs_Texture1, static_cast<void *>(nullptr));
    GxRsSet(GxRs_DepthFunc, GxCmp_LEqual);
    GxRsSet(GxRs_TextureShader0, 0);
    GxRsSet(GxRs_TextureShader1, 0);
    GxRsSet(GxRs_TexGen0, GxTexGen_Disabled);
    GxRsSet(GxRs_TexGen1, GxTexGen_Disabled);
    GxRsSet(GxRs_ColorOp1, GxTexOp_PassThru);
    GxRsSet(GxRs_AlphaOp1, GxTexOp_PassThru);
    GxRsSet(GxRs_TextureCoord0, 0);
    GxRsSet(GxRs_TextureCoord1, 1);
    GxXformSetProjection(projection);
    GxXformSetView(view);
    GxXformSet(GxXform_World, world);
    if (groupSpheres_.size() != groups_.size()) {
        groupSpheres_.assign(groups_.size(), {0, 0, 0, -1});
        for (size_t g = 0; g < groups_.size(); ++g) {
            const auto &v = groups_[g].vertices;
            if (v.empty())
                continue;
            C3Vector lo = v[0].m_position, hi = v[0].m_position;
            for (const auto &x : v) {
                lo = {std::min(lo.x, x.m_position.x), std::min(lo.y, x.m_position.y), std::min(lo.z, x.m_position.z)};
                hi = {std::max(hi.x, x.m_position.x), std::max(hi.y, x.m_position.y), std::max(hi.z, x.m_position.z)};
            }
            const float dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
            groupSpheres_[g] = {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2,
                                .5f * std::sqrt(dx * dx + dy * dy + dz * dz) + .5f};
        }
    }
    const Frustum frustum(world * view * projection);
    for (size_t g = 0; g < groups_.size(); ++g) {
        const auto &group = groups_[g];
        if (group.vertices.empty() || group.indices.empty())
            continue;
        if (const auto &sphere = groupSpheres_[g]; sphere[3] >= 0 && !frustum.sphere(sphere[0], sphere[1], sphere[2], sphere[3]))
            continue;
        const auto &v = group.vertices;
        if (meshes) {
            auto &mesh = *(*meshes)[g];
            if (!mesh.ready()) {
                mesh.setVertices(v.data(), uint32_t(v.size()), sizeof(M2VertexPNT2), GxVBF_PNT2);
                mesh.setIndices(group.indices.data(), uint32_t(group.indices.size()));
            }
            mesh.bind();
        } else {
            GxPrimVertexPtr(v.size(), &v[0].m_position, sizeof(M2VertexPNT2), &v[0].m_normal,
                            sizeof(M2VertexPNT2), nullptr, 0, &v[0].m_texcoord[0], sizeof(M2VertexPNT2),
                            &v[0].m_texcoord[1], sizeof(M2VertexPNT2));
            GxPrimIndexPtr(group.indices.size(), group.indices.data());
        }
        for (const auto &batch : group.batches) {
            if (!batch.indexCount)
                continue;
            const bool textured = textures && batch.texture >= 0;
            // MOMT blend: 0 opaque, 1 alpha key, >= 2 alpha blended.
            GxRsSet(GxRs_BlendingMode, !textured ? GxBlend_Opaque
                                        : batch.blend == 0 ? GxBlend_Opaque
                                        : batch.blend == 1 ? GxBlend_AlphaKey
                                                           : GxBlend_Alpha);
            GxRsSet(GxRs_AlphaRef, textured && batch.blend == 1 ? 224 : 0);
            GxRsSet(GxRs_Texture0, textured ? textures->get(size_t(batch.texture)) : nullptr);
            GxRsSet(GxRs_ColorOp0, textured ? GxTexOp_Mod : GxTexOp_PassThru);
            GxRsSet(GxRs_AlphaOp0, textured ? GxTexOp_Mod : GxTexOp_PassThru);
            GxRsSet(GxRs_MatDiffuse, int32_t(worldLit(textured ? 0xffffffffu : 0xffb0b0b0u)));
            GxRsSet(GxRs_Culling, !(batch.flags & 0x4));
            GxRsSet(GxRs_DepthTest, 1);
            GxRsSet(GxRs_DepthWrite, batch.blend < 2);
            CGxBatch draw{GxPrim_Triangles, batch.firstIndex, batch.indexCount, batch.minIndex,
                          batch.maxIndex};
            GxDraw(&draw, 1);
        }
    }
    GxRsSet(GxRs_AlphaRef, 0);
}
} // namespace wow::android
