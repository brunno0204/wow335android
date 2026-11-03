#pragma once
#include "Engine/Source/Gx/Types.h"
#include <cstdint>
#include <vector>
struct CGxPool;
struct CGxBuf;
namespace wow::android {
// GL-thread-owned persistent vertex/index buffers in original Gx pools. Replaces the
// per-draw GxPrimVertexPtr/GxPrimIndexPtr stream copies for geometry that does not
// change every frame (terrain tiles, WMO groups, static or slowly animated M2 poses).
// Must be destroyed on the GL thread while the Gx device exists.
class GxStaticMesh {
  public:
    GxStaticMesh() = default;
    ~GxStaticMesh();
    GxStaticMesh(const GxStaticMesh &) = delete;
    GxStaticMesh &operator=(const GxStaticMesh &) = delete;
    // Same-size updates rewrite in place; a size change recreates the pool.
    void setVertices(const void *data, uint32_t count, uint32_t stride, EGxVertexBufferFormat format);
    void setIndices(const uint16_t *data, uint32_t count);
    bool ready() const noexcept { return vertexBuffer_ && indexBuffer_; }
    void bind() const; // GxPrimVertexPtr + index buffer for following GxDraw calls
    // Optional per-mesh draw order cache (M2 batches), owned by the user.
    std::vector<size_t> order;
    uint64_t sampledAt = 0;
    int animated = -1; // M2: -1 unknown, 0 static pose, 1 animated
    std::vector<uint8_t> firstSample;

  private:
    void release();
    CGxPool *vertexPool_ = nullptr, *indexPool_ = nullptr;
    CGxBuf *vertexBuffer_ = nullptr, *indexBuffer_ = nullptr;
    uint32_t vertexBytes_ = 0, indexBytes_ = 0;
    EGxVertexBufferFormat format_ = GxVBF_PNT2;
};
} // namespace wow::android
