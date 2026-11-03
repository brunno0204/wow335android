#pragma once
#include "Engine/Source/Tempest/C44Matrix.h"
#include "M2Geometry.h"
#include "M2PreviewMaterials.h"
namespace wow::android {
// Orthographic DEVELOPMENT preview: opaque/key/gray first in stored order,
// then supported transparent batches far-to-near by current section vertex
// centroid in clip depth. Equal depths retain stored order. Not CM2Scene's
// radial/bone/radius/priority-plane/world scene sorting or triangle sorting.
std::vector<size_t> m2PreviewDrawOrder(const M2PreviewMaterials &materials,
                                       const M2Geometry &geometry,
                                       const std::vector<M2VertexPNT2> &vertices,
                                       const C44Matrix &projection,
                                       const std::vector<uint8_t> *visibleSections = nullptr);
} // namespace wow::android
