#pragma once
#include "Engine/Source/Model2/M2Data.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include <cstddef>
#include <vector>

// Original scalar matrix blending (the caller owns palette/index validity).
void M2BlendBoneMatrices(const C44Matrix *bones, ubyte4 weights, ubyte4 indices, C44Matrix *result);

namespace M2Skinning {
// Checked native path for prepared M2 vertices with GLOBAL bone indices.
// CM2Shared's skin-profile remapping must happen before this function. Weights
// are packed, byte/255 and not renormalized. Normals use the original linear
// blend, without normalization. The one-bone optimization uses the full matrix.
// Returns owned PNT2 vertices; invalid input never modifies caller-owned data.
// This does not evaluate animation tracks or parse M2/SKIN files.
std::vector<M2VertexPNT2> Transform(const M2Vertex *vertices, size_t vertexCount, const C44Matrix *bones,
                                    size_t boneCount, bool oneBone = false);
} // namespace M2Skinning
