#include "GxStaticMesh.h"
#include "Engine/Source/Gx/CGxBuf.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
namespace wow::android {
GxStaticMesh::~GxStaticMesh() { release(); }
void GxStaticMesh::release() {
    if (!g_theGxDevicePtr)
        return;
    if (vertexBuffer_)
        g_theGxDevicePtr->BufDestroy(vertexBuffer_);
    if (indexBuffer_)
        g_theGxDevicePtr->BufDestroy(indexBuffer_);
    if (vertexPool_)
        g_theGxDevicePtr->PoolDestroy(vertexPool_);
    if (indexPool_)
        g_theGxDevicePtr->PoolDestroy(indexPool_);
    vertexPool_ = indexPool_ = nullptr;
    vertexBytes_ = indexBytes_ = 0;
}
void GxStaticMesh::setVertices(const void *data, uint32_t count, uint32_t stride,
                               EGxVertexBufferFormat format) {
    const uint32_t bytes = count * stride;
    if (!bytes)
        return;
    format_ = format;
    if (bytes != vertexBytes_) {
        if (vertexBuffer_)
            g_theGxDevicePtr->BufDestroy(vertexBuffer_);
        if (vertexPool_)
            g_theGxDevicePtr->PoolDestroy(vertexPool_);
        vertexPool_ = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Vertex, GxPoolUsage_Static, int32_t(bytes),
                                                   GxPoolHintBit_Unk0, "AndroidStaticVtx");
        vertexBuffer_ = g_theGxDevicePtr->BufCreate(vertexPool_, stride, count, 0);
        vertexBytes_ = bytes;
    }
    g_theGxDevicePtr->BufData(vertexBuffer_, data, bytes, 0);
}
void GxStaticMesh::setIndices(const uint16_t *data, uint32_t count) {
    const uint32_t bytes = count * 2;
    if (!bytes)
        return;
    if (bytes != indexBytes_) {
        if (indexBuffer_)
            g_theGxDevicePtr->BufDestroy(indexBuffer_);
        if (indexPool_)
            g_theGxDevicePtr->PoolDestroy(indexPool_);
        indexPool_ = g_theGxDevicePtr->PoolCreate(GxPoolTarget_Index, GxPoolUsage_Static, int32_t(bytes),
                                                  GxPoolHintBit_Unk0, "AndroidStaticIdx");
        indexBuffer_ = g_theGxDevicePtr->BufCreate(indexPool_, 2, count, 0);
        indexBytes_ = bytes;
    }
    g_theGxDevicePtr->BufData(indexBuffer_, data, bytes, 0);
}
void GxStaticMesh::bind() const {
    GxPrimVertexPtr(vertexBuffer_, format_);
    g_theGxDevicePtr->PrimIndexPtr(indexBuffer_);
}
} // namespace wow::android
