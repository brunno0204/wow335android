#include "Engine/Source/Model2/M2Skinning.h"
#include "Engine/Source/Model2/M2Model.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

// VERIFY: f7794afd auto
// OFFSET: 0x0081D3D0
void M2BlendBoneMatrices(const C44Matrix *bones, ubyte4 weights, ubyte4 indices, C44Matrix *result) {
    const C44Matrix *bone = &bones[indices.b[0]];
    float weight = weights.b[0] * 0.0039215689f;
    result->a0 = bone->a0 * weight;
    result->a1 = bone->a1 * weight;
    result->a2 = bone->a2 * weight;
    result->b0 = bone->b0 * weight;
    result->b1 = bone->b1 * weight;
    result->b2 = bone->b2 * weight;
    result->c0 = bone->c0 * weight;
    result->c1 = bone->c1 * weight;
    result->c2 = bone->c2 * weight;
    result->d0 = bone->d0 * weight;
    result->d1 = bone->d1 * weight;
    result->d2 = weight * bone->d2;

    for (uint32_t i = 1; i < 4; i++) {
        if (!weights.b[i]) {
            break;
        }

        bone = &bones[indices.b[i]];
        weight = weights.b[i] * 0.0039215689f;
        result->a0 = bone->a0 * weight + result->a0;
        result->a1 = bone->a1 * weight + result->a1;
        result->a2 = bone->a2 * weight + result->a2;
        result->b0 = bone->b0 * weight + result->b0;
        result->b1 = bone->b1 * weight + result->b1;
        result->b2 = bone->b2 * weight + result->b2;
        result->c0 = bone->c0 * weight + result->c0;
        result->c1 = bone->c1 * weight + result->c1;
        result->c2 = bone->c2 * weight + result->c2;
        result->d0 = bone->d0 * weight + result->d0;
        result->d1 = bone->d1 * weight + result->d1;
        result->d2 = weight * bone->d2 + result->d2;
    }
}

#if !defined(_WIN32)
namespace {
std::vector<M2VertexPNT2> nativeSkinSection(const CM2Model *model, const M2SkinSection *section,
                                            bool oneBone) {
    if (!model || !section || !model->m_shared || !model->m_shared->m_data)
        throw std::invalid_argument("Missing M2 skinning state");
    const auto &data = *model->m_shared->m_data;
    if (section->m_vertexStart > data.m_vertices.m_count ||
        section->m_vertexCount > data.m_vertices.m_count - section->m_vertexStart)
        throw std::invalid_argument("M2 skin section exceeds vertices");
    if (!section->m_vertexCount)
        return {};
    return M2Skinning::Transform(data.m_vertices.Data() + section->m_vertexStart, section->m_vertexCount,
                                 model->m_boneMatrices, data.m_bones.m_count, oneBone);
}
void nativeSkinPNT(const CM2Model *model, const M2SkinSection *section, uint8_t *output, int32_t uv,
                   bool oneBone) {
    if (uv < 0 || uv > 1 || (!output && section && section->m_vertexCount))
        throw std::invalid_argument("Invalid M2 PNT output/UV set");
    const auto vertices = nativeSkinSection(model, section, oneBone);
    for (size_t i = 0; i < vertices.size(); ++i) {
        const auto &v = vertices[i];
        const float pnt[8]{v.m_position.x, v.m_position.y, v.m_position.z,     v.m_normal.x,
                           v.m_normal.y,   v.m_normal.z,   v.m_texcoord[uv].x, v.m_texcoord[uv].y};
        std::memcpy(output + i * sizeof(pnt), pnt, sizeof(pnt));
    }
}
} // namespace
#endif

// VERIFY: 0efd8ae8 manual
// OFFSET: 0x00829F40
void CM2Model::TransformVerticesNoUVSelect_cpp(const CM2Model *model, const M2SkinSection *section,
                                               M2VertexPNT2 *out) {
#if defined(_WIN32)
    C44Matrix matrix;
    uint32_t weights = 0;
    uint32_t indices = 0;
    const C44Matrix *bones = model->m_boneMatrices;
    M2Vertex *vertex = &model->m_shared->m_data->m_vertices[section->m_vertexStart];
    M2Vertex *end = vertex + section->m_vertexCount;

    for (; vertex < end; vertex++, out++) {
        if (*reinterpret_cast<uint32_t *>(&vertex->m_weights) != weights ||
            *reinterpret_cast<uint32_t *>(&vertex->m_indices) != indices) {
            weights = *reinterpret_cast<uint32_t *>(&vertex->m_weights);
            indices = *reinterpret_cast<uint32_t *>(&vertex->m_indices);

            const C44Matrix &bone = bones[vertex->m_indices.b[0]];
            float weight = vertex->m_weights.b[0] * 0.0039215689f;
            matrix.a0 = bone.a0 * weight;
            matrix.a1 = bone.a1 * weight;
            matrix.a2 = bone.a2 * weight;
            matrix.b0 = bone.b0 * weight;
            matrix.b1 = bone.b1 * weight;
            matrix.b2 = bone.b2 * weight;
            matrix.c0 = bone.c0 * weight;
            matrix.c1 = bone.c1 * weight;
            matrix.c2 = bone.c2 * weight;
            matrix.d0 = bone.d0 * weight;
            matrix.d1 = bone.d1 * weight;
            matrix.d2 = bone.d2 * weight;

            for (uint32_t k = 1; k < 4 && vertex->m_weights.b[k]; k++) {
                const C44Matrix &next = bones[vertex->m_indices.b[k]];
                weight = vertex->m_weights.b[k] * 0.0039215689f;
                matrix.a0 += next.a0 * weight;
                matrix.a1 += next.a1 * weight;
                matrix.a2 += next.a2 * weight;
                matrix.b0 += next.b0 * weight;
                matrix.b1 += next.b1 * weight;
                matrix.b2 += next.b2 * weight;
                matrix.c0 += next.c0 * weight;
                matrix.c1 += next.c1 * weight;
                matrix.c2 += next.c2 * weight;
                matrix.d0 += next.d0 * weight;
                matrix.d1 += next.d1 * weight;
                matrix.d2 += next.d2 * weight;
            }
        }

        out->m_position = vertex->m_position * matrix;
        out->m_normal.x =
            vertex->m_normal.z * matrix.c0 + vertex->m_normal.x * matrix.a0 + matrix.b0 * vertex->m_normal.y;
        out->m_normal.y =
            vertex->m_normal.z * matrix.c1 + vertex->m_normal.x * matrix.a1 + matrix.b1 * vertex->m_normal.y;
        out->m_normal.z =
            vertex->m_normal.z * matrix.c2 + vertex->m_normal.x * matrix.a2 + matrix.b2 * vertex->m_normal.y;
        out->m_texcoord[0] = vertex->m_texcoord[0];
        out->m_texcoord[1] = vertex->m_texcoord[1];
    }
#else
    if (!out && section && section->m_vertexCount)
        throw std::invalid_argument("Null M2 output");
    const auto transformed = nativeSkinSection(model, section, false);
    if (!transformed.empty())
        std::memcpy(out, transformed.data(), transformed.size() * sizeof(M2VertexPNT2));
#endif
}

// VERIFY: 3101ba14 manual
// OFFSET: 0x0082A210
void CM2Model::TransformVertices_cpp(CM2Model *model, M2SkinSection *section, uint8_t *vertices,
                                     int32_t texCoord) {
#if defined(_WIN32)
    C44Matrix matrix;
    uint32_t weights = 0;
    uint32_t indices = 0;
    const C44Matrix *bones = model->m_boneMatrices;
    M2Vertex *vertex = &model->m_shared->m_data->m_vertices[section->m_vertexStart];
    M2Vertex *end = vertex + section->m_vertexCount;
    float *out = reinterpret_cast<float *>(vertices);

    for (; vertex < end; vertex++, out += 8) {
        if (*reinterpret_cast<uint32_t *>(&vertex->m_weights) != weights ||
            *reinterpret_cast<uint32_t *>(&vertex->m_indices) != indices) {
            weights = *reinterpret_cast<uint32_t *>(&vertex->m_weights);
            indices = *reinterpret_cast<uint32_t *>(&vertex->m_indices);

            const C44Matrix &bone = bones[vertex->m_indices.b[0]];
            float weight = vertex->m_weights.b[0] * 0.0039215689f;
            matrix.a0 = bone.a0 * weight;
            matrix.a1 = bone.a1 * weight;
            matrix.a2 = bone.a2 * weight;
            matrix.b0 = bone.b0 * weight;
            matrix.b1 = bone.b1 * weight;
            matrix.b2 = bone.b2 * weight;
            matrix.c0 = bone.c0 * weight;
            matrix.c1 = bone.c1 * weight;
            matrix.c2 = bone.c2 * weight;
            matrix.d0 = bone.d0 * weight;
            matrix.d1 = bone.d1 * weight;
            matrix.d2 = bone.d2 * weight;

            for (uint32_t k = 1; k < 4 && vertex->m_weights.b[k]; k++) {
                const C44Matrix &next = bones[vertex->m_indices.b[k]];
                weight = vertex->m_weights.b[k] * 0.0039215689f;
                matrix.a0 += next.a0 * weight;
                matrix.a1 += next.a1 * weight;
                matrix.a2 += next.a2 * weight;
                matrix.b0 += next.b0 * weight;
                matrix.b1 += next.b1 * weight;
                matrix.b2 += next.b2 * weight;
                matrix.c0 += next.c0 * weight;
                matrix.c1 += next.c1 * weight;
                matrix.c2 += next.c2 * weight;
                matrix.d0 += next.d0 * weight;
                matrix.d1 += next.d1 * weight;
                matrix.d2 += next.d2 * weight;
            }
        }

        C3Vector position = vertex->m_position * matrix;
        out[0] = position.x;
        out[1] = position.y;
        out[2] = position.z;
        out[3] =
            vertex->m_normal.y * matrix.b0 + matrix.c0 * vertex->m_normal.z + matrix.a0 * vertex->m_normal.x;
        out[4] =
            vertex->m_normal.y * matrix.b1 + matrix.c1 * vertex->m_normal.z + matrix.a1 * vertex->m_normal.x;
        out[5] =
            vertex->m_normal.y * matrix.b2 + matrix.c2 * vertex->m_normal.z + matrix.a2 * vertex->m_normal.x;
        out[6] = vertex->m_texcoord[texCoord].x;
        out[7] = vertex->m_texcoord[texCoord].y;
    }
#else
    nativeSkinPNT(model, section, vertices, texCoord, false);
#endif
}

// VERIFY: 3dfd45aa manual
// OFFSET: 0x0082A4E0
void CM2Model::TransformVerticesOneBone_cpp(CM2Model *model, M2SkinSection *section, uint8_t *vertices,
                                            int32_t texCoord) {
#if defined(_WIN32)
    const C44Matrix *bones = model->m_boneMatrices;
    M2Vertex *vertex = &model->m_shared->m_data->m_vertices[section->m_vertexStart];
    M2Vertex *end = vertex + section->m_vertexCount;
    float *out = reinterpret_cast<float *>(vertices);

    for (; vertex < end; vertex++, out += 8) {
        const C44Matrix &bone = bones[vertex->m_indices.b[0]];
        out[0] = bone.c0 * vertex->m_position.z + vertex->m_position.y * bone.b0 +
                 vertex->m_position.x * bone.a0 + bone.d0;
        out[1] = vertex->m_position.x * bone.a1 + vertex->m_position.y * bone.b1 +
                 bone.c1 * vertex->m_position.z + bone.d1;
        out[2] = vertex->m_position.x * bone.a2 + vertex->m_position.y * bone.b2 +
                 bone.c2 * vertex->m_position.z + bone.d2;
        out[3] = vertex->m_normal.x * bone.a0 + bone.c0 * vertex->m_normal.z + bone.b0 * vertex->m_normal.y;
        out[4] = bone.c1 * vertex->m_normal.z + bone.b1 * vertex->m_normal.y + bone.a1 * vertex->m_normal.x;
        out[5] = bone.c2 * vertex->m_normal.z + bone.b2 * vertex->m_normal.y + bone.a2 * vertex->m_normal.x;
        out[6] = vertex->m_texcoord[texCoord].x;
        out[7] = vertex->m_texcoord[texCoord].y;
    }
#else
    nativeSkinPNT(model, section, vertices, texCoord, true);
#endif
}

namespace M2Skinning {
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
void finite(const C3Vector &value) {
    require(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z),
            "Nonfinite M2 vertex/normal");
}
void finite(const C44Matrix &m) {
    const float affine[12]{m.a0, m.a1, m.a2, m.b0, m.b1, m.b2, m.c0, m.c1, m.c2, m.d0, m.d1, m.d2};
    for (float f : affine)
        require(std::isfinite(f), "Nonfinite M2 bone matrix");
}
} // namespace
std::vector<M2VertexPNT2> Transform(const M2Vertex *vertices, size_t vertexCount, const C44Matrix *bones,
                                    size_t boneCount, bool oneBone) {
    require(vertexCount <= 1024 * 1024, "M2 skinning vertex budget exceeded");
    if (!vertexCount)
        return {};
    require(vertices && bones && boneCount && boneCount <= 65536, "Invalid M2 skinning buffers");
    // Validate all source attributes/influences before any indexed palette read.
    for (size_t i = 0; i < vertexCount; ++i) {
        const auto &v = vertices[i];
        finite(v.m_position);
        finite(v.m_normal);
        for (const auto &uv : v.m_texcoord)
            require(std::isfinite(uv.x) && std::isfinite(uv.y), "Nonfinite M2 UV");
        require(v.m_weights.b[0] > 0, "M2 vertex has no first bone influence");
        bool ended = false;
        for (unsigned k = 0; k < 4; ++k) {
            if (!v.m_weights.b[k]) {
                ended = true;
                continue;
            }
            require(!ended, "M2 bone weights must be packed");
            if (oneBone)
                require(k == 0 && v.m_weights.b[0] == 255, "Invalid single-bone M2 weights");
            require(v.m_indices.b[k] < boneCount, "M2 bone index exceeds palette");
        }
    }
    std::vector<M2VertexPNT2> output(vertexCount);
    C44Matrix matrix(1);
    uint32_t previousWeights = 0, previousIndices = 0;
    bool haveMatrix = false;
    for (size_t i = 0; i < vertexCount; ++i) {
        const auto &v = vertices[i];
        auto &out = output[i];
        uint32_t weights, indices;
        std::memcpy(&weights, &v.m_weights, sizeof(weights));
        std::memcpy(&indices, &v.m_indices, sizeof(indices));
        if (!haveMatrix || weights != previousWeights || indices != previousIndices) {
            for (unsigned k = 0; k < 4 && v.m_weights.b[k]; ++k)
                finite(bones[v.m_indices.b[k]]);
            if (oneBone)
                matrix = bones[v.m_indices.b[0]];
            else
                M2BlendBoneMatrices(bones, v.m_weights, v.m_indices, &matrix);
            finite(matrix);
            previousWeights = weights;
            previousIndices = indices;
            haveMatrix = true;
        }
        out.m_position = v.m_position * matrix;
        out.m_normal = {v.m_normal.x * matrix.a0 + v.m_normal.y * matrix.b0 + v.m_normal.z * matrix.c0,
                        v.m_normal.x * matrix.a1 + v.m_normal.y * matrix.b1 + v.m_normal.z * matrix.c1,
                        v.m_normal.x * matrix.a2 + v.m_normal.y * matrix.b2 + v.m_normal.z * matrix.c2};
        finite(out.m_position);
        finite(out.m_normal);
        out.m_texcoord[0] = v.m_texcoord[0];
        out.m_texcoord[1] = v.m_texcoord[1];
    }
    return output;
}
} // namespace M2Skinning
