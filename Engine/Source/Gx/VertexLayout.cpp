#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Gx/VertexLayoutData.h"
using namespace GxVertexLayout;

// VERIFY: 619cba02 auto
// OFFSET: 0x00681230
uint32_t GxVertexSize(EGxVertexBufferFormat format) {
    return s_vertexSize[format];
}

// VERIFY: d108cfe2 auto
// OFFSET: 0x00681240
uint32_t GxVertexAttribOffset(EGxVertexBufferFormat format, EGxVertexAttrib attrib) {
    return s_vertexAttribOffset[format][attrib];
}

// VERIFY: 6233e69b auto
// OFFSET: 0x00681260
int32_t GxVertexHasAttrib(EGxVertexBufferFormat format, EGxVertexAttrib attrib) {
    return s_vertexAttribOffset[format][attrib] != -1;
}

