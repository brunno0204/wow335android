#pragma once
#include "Engine/Source/Gx/CGxDevice/Types.h"
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CImVector.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace wow::android {
// Native GPU core used by CGxDeviceGles. The fixed material path supports
// unlit and four-light vertex lighting; legacy game shaders remain separate.
class GlesRenderer {
  public:
    struct Buffer {
        uint64_t value;
        constexpr Buffer(uint64_t id = 0) : value(id) {}
    };
    struct Texture {
        uint64_t value;
        constexpr Texture(uint64_t id = 0) : value(id) {}
    };
    struct Image {
        uint32_t face = 0, level = 0, stride = 0;
        std::vector<uint8_t> bytes;
    };
    struct TextureDesc {
        EGxTexTarget target = GxTex_2d;
        EGxTexFormat format = GxTex_Abgr8888;
        uint32_t width = 0, height = 0;
        EGxTexFilter filter = GxTex_Linear;
        bool wrapU = false, wrapV = false, generateMips = false, renderTarget = false;
        std::vector<Image> images;
    };
    struct Light {
        bool enabled = false;
        float position[4]{0, 0, 1, 0}; // Eye space; w=0 directional, w=1 point.
        float ambient[3]{}, diffuse[3]{}, specular[3]{};
        float attenuation[3]{1, 0, 0};
    };
    struct DrawState {
        bool lighting = false, normalizeNormals = false;
        C44Matrix modelView{1};
        // Row-vector inverse transpose, uploaded as a column-major GL matrix.
        float normalTransform[9]{1, 0, 0, 0, 1, 0, 0, 0, 1};
        Light lights[4];
        CImVector specularColor{0, 0, 0, 255}, emissiveColor{0, 0, 0, 255};
        float shininess = 0;

        EGxBlend blend = GxBlend_Opaque;
        // Keep raw render-state integers until validation (invalid enum loads
        // themselves can be undefined behavior before a range check).
        int32_t colorOp = GxTexOp_Mod, alphaOp = GxTexOp_Mod;
        bool depthTest = false, depthWrite = false, cullBackFaces = false, cullClockwise = false;
        EGxCmp depthFunc = GxCmp_LEqual;
        uint32_t colorWriteMask = 15;
        float polygonOffset = 0.0f;
        float alphaRef = -1.0f;
        CImVector constantColor{255, 255, 255, 255};
        C44Matrix textureTransform;
        Texture secondTexture;
        C44Matrix secondTextureTransform;
        int32_t secondColorOp = GxTexOp_Mod, secondAlphaOp = GxTexOp_Mod;
        int32_t textureCoord = 0, secondTextureCoord = 1;
        bool fog = false;
        float fogStart = 0.0f, fogEnd = 1.0f;
        CImVector fogColor{128, 128, 128, 255};
        // Eye-space Z column (before projection); fixed GL fog uses abs(eye Z).
        float fogDepthTransform[4]{0, 0, 1, 0};
        bool viewport = false;
        int viewportX = 0, viewportY = 0, viewportWidth = 0, viewportHeight = 0;
        float minDepth = 0.0f, maxDepth = 1.0f;
        bool scissor = false;
        int x = 0, y = 0, width = 0, height = 0; // Bottom-left pixels.
        float blendFactor = 1.0f;
    };
    GlesRenderer();
    ~GlesRenderer();
    GlesRenderer(const GlesRenderer &) = delete;
    GlesRenderer &operator=(const GlesRenderer &) = delete;

    // Call on the GL thread with a current EGL context. CPU copies survive loss;
    // GPU buffers/textures/programs are restored lazily in the next context.
    void attachCurrentContext();
    void releaseCurrentContext();
    void loseContext(); // No GL calls: valid after a context has already vanished.
    void resize(uint32_t width, uint32_t height);
    Buffer createBuffer(EGxPoolTarget target, EGxPoolUsage usage, const void *data, size_t bytes);
    void updateBuffer(Buffer buffer, size_t offset, const void *data, size_t bytes);
    void destroyBuffer(Buffer buffer);
    Texture createTexture(const TextureDesc &description);
    void updateTexture(Texture texture, const Image &image, EGxTexFormat format, uint32_t x,
                       uint32_t y, uint32_t width, uint32_t height);
    void destroyTexture(Texture texture);
    void setTextureSampler(Texture texture, EGxTexFilter filter, bool wrapU, bool wrapV);
    void setRenderTarget(Texture texture = {}, uint32_t face = 0);
    void clear(CImVector color, bool depth = true);
    void clear(uint32_t mask, CImVector color);
    void drawUnlit(Buffer vertices, Buffer indices, EGxVertexBufferFormat format, EGxPrim primitive,
                   uint32_t firstIndex, uint32_t indexCount, Texture texture,
                   const C44Matrix &transform, const DrawState &state, uint32_t baseVertex = 0,
                   size_t vertexByteOffset = 0, uint32_t vertexCountLimit = 0);
    std::vector<uint8_t> readPixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace wow::android
