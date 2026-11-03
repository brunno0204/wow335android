#include "CGxDeviceGles.h"
#include "Engine/Source/Gx/CGxPool.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/ShaderEffect/Types.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Tempest/C33Matrix.h"
#include "GlesRenderer.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using GPU = wow::android::GlesRenderer;
void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
[[noreturn]] void unsupported(const char *feature) {
    SErrSetLastError(50);
    throw std::runtime_error(std::string("GLES Gx feature not implemented: ") + feature);
}
C44Matrix nativeProjection(const C44Matrix &matrix, bool normal, bool target) {
    C44Matrix projection(matrix);
    projection.c0 = -projection.c0;
    projection.c1 = -projection.c1;
    projection.c2 = -projection.c2;
    projection.c3 = -projection.c3;
    if (!normal && matrix.d3 != 1.0f) {
        C44Matrix scale(.2f);
        projection = projection * scale;
    }
    if (target) {
        projection.a1 = -projection.a1;
        projection.b1 = -projection.b1;
        projection.c1 = -projection.c1;
        projection.d1 = -projection.d1;
    }
    return projection;
}
EGLConfig currentConfig() {
    EGLint id = 0, count = 0;
    const auto display = eglGetCurrentDisplay();
    if (!eglQueryContext(display, eglGetCurrentContext(), EGL_CONFIG_ID, &id))
        throw std::runtime_error("Cannot query EGL configuration");
    const EGLint attributes[] = {EGL_CONFIG_ID, id, EGL_NONE};
    EGLConfig config{};
    if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1)
        throw std::runtime_error("Cannot resolve EGL configuration");
    return config;
}
int configValue(EGLint key) {
    EGLint value = 0;
    if (!eglGetConfigAttrib(eglGetCurrentDisplay(), currentConfig(), key, &value))
        throw std::runtime_error("Cannot query EGL format");
    return value;
}
bool surfaceSize(int &width, int &height) {
    return eglGetCurrentSurface(EGL_DRAW) != EGL_NO_SURFACE &&
           eglQuerySurface(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW), EGL_WIDTH,
                           &width) &&
           eglQuerySurface(eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW), EGL_HEIGHT,
                           &height) &&
           width > 0 && height > 0;
}
} // namespace
struct CGxDeviceGles::Backend {
    struct Pool {
        GPU::Buffer handle;
        std::vector<uint8_t> bytes;
    };
    struct Texture {
        GPU::Texture handle;
        uint32_t baseMip = 0;
        EGxTexFormat dataFormat = GxTex_Unknown;
        std::optional<CGxTexFlags> flags;
    };
    GPU gpu;
    std::map<CGxPool *, Pool> pools;
    std::map<CGxTex *, Texture> textures;
    EGLContext context = EGL_NO_CONTEXT;
    std::thread::id thread;
    bool attached = false;
    void current() const {
        if (!attached || context != eglGetCurrentContext() || thread != std::this_thread::get_id())
            throw std::logic_error("Original Gx requires its current EGL context/thread");
    }
    Pool &pool(CGxPool *pool) {
        require(pool && pool->m_size > 0, "Invalid native Gx pool");
        auto &result = pools[pool];
        if (!result.handle.value) {
            result.bytes.resize(pool->m_size);
            result.handle =
                gpu.createBuffer(pool->m_target, pool->m_usage, nullptr, result.bytes.size());
            pool->m_apiSpecific = &result;
            pool->m_mem = nullptr;
        }
        return result;
    }
    void reserve(CGxBuf *buffer) {
        auto *pool = buffer->m_pool;
        require(buffer->m_size <= uint32_t(pool->m_size), "Gx buffer exceeds its pool");
        if (pool->m_usage == GxPoolUsage_Stream) {
            uint64_t next = (uint64_t(pool->m_offset) + 15) & ~uint64_t(15);
            if (next + buffer->m_size > uint32_t(pool->m_size))
                next = 0;
            buffer->m_index = uint32_t(next);
            pool->m_offset = int32_t(next + buffer->m_size);
        }
        require(buffer->m_index <= uint32_t(pool->m_size) &&
                    buffer->m_size <= uint32_t(pool->m_size) - buffer->m_index,
                "Gx buffer view exceeds its pool");
    }
    bool owns(CGxDeviceGles &device, CGxTex *texture) {
        for (auto *candidate = device.m_textures.Head(); candidate;
             candidate = device.m_textures.Next(candidate))
            if (candidate == texture)
                return true;
        return false;
    }
    GPU::Texture upload(CGxDeviceGles &device, CGxTex *texture) {
        require(texture && owns(device, texture), "Texture does not belong to this Gx device");
        require(texture->m_width && texture->m_height && texture->m_width <= 4096 &&
                    texture->m_height <= 4096 && texture->m_depth <= 1 &&
                    texture->m_dataFormat > GxTex_Unknown && texture->m_dataFormat <= GxTex_Dxt5,
                "Invalid Gx texture dimensions/depth");
        if (texture->m_flags.m_forceMipTracking)
            unsupported("forced mip tracking");
        uint32_t levels = 1;
        for (uint32_t n = std::max(texture->m_width, texture->m_height); n > 1; n >>= 1)
            ++levels;
        const bool mipFilter = texture->m_flags.m_filter >= GxTex_NearestMipNearest;
        const uint32_t baseMip =
            (!texture->m_flags.m_generateMipMaps && mipFilter && !texture->m_flags.m_renderTarget)
                ? std::min(device.m_baseMipLevel, levels - 1)
                : 0;
        auto &record = textures[texture];
        bool flagsChanged = !record.flags || *record.flags != texture->m_flags;
        if (record.handle.value && flagsChanged && !texture->m_needsCreation &&
            !texture->m_needsUpdate && record.flags && record.baseMip == baseMip &&
            record.dataFormat == texture->m_dataFormat &&
            record.flags->m_generateMipMaps == texture->m_flags.m_generateMipMaps &&
            record.flags->m_renderTarget == texture->m_flags.m_renderTarget &&
            (record.flags->m_filter >= GxTex_NearestMipNearest) == mipFilter) {
            gpu.setTextureSampler(record.handle,
                                  static_cast<EGxTexFilter>(texture->m_flags.m_filter),
                                  texture->m_flags.m_wrapU, texture->m_flags.m_wrapV);
            record.flags = texture->m_flags;
            texture->m_needsFlagUpdate = 0;
            flagsChanged = false;
        }
        if (record.handle.value && texture->m_flags.m_renderTarget && !texture->m_needsCreation &&
            !flagsChanged) {
            device.CGxDevice::ITexMarkAsUpdated(texture);
            return record.handle;
        }
        if (record.handle.value && !texture->m_needsCreation && !texture->m_needsUpdate &&
            !flagsChanged && record.baseMip == baseMip &&
            record.dataFormat == texture->m_dataFormat) {
            texture->m_needsFlagUpdate = 0;
            return record.handle;
        }
        GPU::TextureDesc description;
        description.target = texture->m_target;
        description.format = texture->m_dataFormat;
        description.width = std::max(1u, texture->m_width >> baseMip);
        description.height = std::max(1u, texture->m_height >> baseMip);
        description.filter = static_cast<EGxTexFilter>(texture->m_flags.m_filter);
        description.wrapU = texture->m_flags.m_wrapU;
        description.wrapV = texture->m_flags.m_wrapV;
        description.generateMips = texture->m_flags.m_generateMipMaps;
        description.renderTarget = texture->m_flags.m_renderTarget;
        if (!description.renderTarget) {
            require(texture->m_userFunc, "Gx texture needs a source callback");
            uint32_t stride = 0;
            const void *texels = nullptr;
            texture->m_userFunc(GxTex_Lock, texture->m_width, texture->m_height, 0, 0,
                                texture->m_userArg, stride, texels);
            struct Unlock {
                CGxTex *texture;
                uint32_t &stride;
                const void *&texels;
                bool done = false;
                void close() {
                    done = true;
                    texture->m_userFunc(GxTex_Unlock, texture->m_width, texture->m_height, 0, 0,
                                        texture->m_userArg, stride, texels);
                }
                ~Unlock() {
                    if (!done)
                        try {
                            close();
                        } catch (...) {
                        }
                }
            } unlock{texture, stride, texels};
            const uint32_t faces = texture->m_target == GxTex_CubeMap ? 6 : 1;
            const uint32_t end = (mipFilter && !description.generateMips) ? levels : baseMip + 1;
            for (uint32_t face = 0; face < faces; ++face)
                for (uint32_t level = baseMip; level < end; ++level) {
                    const uint32_t w = std::max(1u, texture->m_width >> level),
                                   h = std::max(1u, texture->m_height >> level);
                    stride = 0;
                    texels = nullptr;
                    texture->m_userFunc(GxTex_Latch, w, h, face, level, texture->m_userArg, stride,
                                        texels);
                    require(texels, "Gx texture callback returned no pixels");
                    const uint32_t rowBytes = GxCalcTexelStrideInBytes(texture->m_dataFormat, w);
                    const uint32_t rows =
                        (texture->m_dataFormat >= GxTex_Dxt1 && texture->m_dataFormat <= GxTex_Dxt5)
                            ? (h + 3) / 4
                            : h;
                    require(stride >= rowBytes && rowBytes, "Invalid Gx texture callback pitch");
                    const uint64_t bytes = uint64_t(rows - 1) * stride + rowBytes;
                    require(bytes <= 128 * 1024 * 1024,
                            "Gx texture callback pitch exceeds staging limit");
                    GPU::Image image;
                    image.face = face;
                    image.level = level - baseMip;
                    image.stride = stride;
                    const auto *data = static_cast<const uint8_t *>(texels);
                    image.bytes.assign(data, data + bytes);
                    description.images.push_back(std::move(image));
                }
            unlock.close();
        }
        const auto replacement = gpu.createTexture(description);
        if (record.handle.value)
            gpu.destroyTexture(record.handle);
        record.handle = replacement;
        record.baseMip = baseMip;
        record.dataFormat = texture->m_dataFormat;
        record.flags = texture->m_flags;
        texture->m_apiSpecificData = &record;
        texture->m_needsCreation = 0;
        texture->m_needsFlagUpdate = 0;
        device.CGxDevice::ITexMarkAsUpdated(texture);
        return record.handle;
    }
    void target(CGxDeviceGles &device) {
        if (auto *texture = device.m_textureTarget[GxBuffers_Color].m_texture)
            gpu.setRenderTarget(upload(device, texture),
                                device.m_textureTarget[GxBuffers_Color].m_plane);
        else
            gpu.setRenderTarget();
    }
};
CGxDeviceGles::CGxDeviceGles() : backend(std::make_unique<Backend>()) {}
CGxDeviceGles::~CGxDeviceGles() {
    m_streamBufs[0] = m_streamBufs[1] = nullptr;
    for (auto *pool = m_poolList.Head(); pool; pool = m_poolList.Next(pool))
        while (auto *buffer = pool->m_bufList.Head()) {
            CGxBuf *value = buffer;
            CGxDevice::BufDestroy(value);
        }
    while (auto *texture = m_textures.Head()) {
        texture->m_apiSpecificData = nullptr;
        CGxDevice::TexDestroy(texture);
    }
    while (auto *query = m_queryList.Head()) {
        CGxQuery *value = query;
        CGxDevice::QueryDestroy(value);
    }
    for (auto &pair : backend->pools) {
        pair.first->m_apiSpecific = nullptr;
        pair.first->m_mem = nullptr;
    }
}
int32_t CGxDeviceGles::DeviceCreate(GxWindowProc callback, const CGxFormat &format) {
    backend->gpu.attachCurrentContext();
    backend->attached = true;
    backend->context = eglGetCurrentContext();
    backend->thread = std::this_thread::get_id();
    m_windowProc = callback;
    m_api = GxApi_OpenGl;
    memset(&m_caps, 0, sizeof(m_caps));
    m_caps.m_numTmus = 2;
    m_caps.m_numStreams = 1;
    m_caps.m_streamOffset = 1;
    m_caps.m_maxIndex = 65535;
    m_caps.m_colorFormat = GxCF_argb;
    m_caps.m_generateMipMaps = 1;
    m_caps.m_texFilterTrilinear = 1;
    m_caps.m_maxTexAnisotropy = 1;
    m_caps.m_colorWrite = 1;
    m_caps.m_shaderAlphaRef = 1;
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    for (auto target : {GxTex_2d, GxTex_NonPow2}) {
        m_caps.m_texTarget[target] = 1;
        m_caps.m_texMaxSize[target] = std::min(4096, maximum);
    }
    for (int format = GxTex_Abgr8888; format <= GxTex_Dxt5; ++format)
        m_caps.m_texFmt[format] = 1;
    m_caps.m_rttFormat[GxTex_Abgr8888] = m_caps.m_rttFormat[GxTex_Argb8888] = 1;
    m_context = 1;
    m_windowVisible = m_windowFocus = 1;
    if (!DeviceSetFormat(format)) {
        DeviceDestroy();
        return 0;
    }
    DeviceCreatePools();
    DeviceCreateStreamBufs();
    return 1;
}
int32_t CGxDeviceGles::DeviceCreate(void *surface, const CGxFormat &format) {
    if (surface && surface != eglGetCurrentSurface(EGL_DRAW)) {
        SErrSetLastError(87);
        return 0;
    }
    return DeviceCreate(static_cast<GxWindowProc>(nullptr), format);
}
void CGxDeviceGles::DeviceDestroy() {
    if (backend->attached) {
        if (backend->context == eglGetCurrentContext() &&
            backend->thread == std::this_thread::get_id())
            backend->gpu.releaseCurrentContext();
        else
            backend->gpu.loseContext();
        backend->attached = false;
        backend->context = EGL_NO_CONTEXT;
    }
    m_context = 0;
    CGxDevice::DeviceDestroy();
}
int32_t CGxDeviceGles::DeviceSetFormat(const CGxFormat &format) {
    backend->current();
    int width = 0, height = 0;
    if (!surfaceSize(width, height) || (format.m_size.x > 0 && format.m_size.x != width) ||
        (format.m_size.y > 0 && format.m_size.y != height) || format.m_stereoEnabled) {
        SErrSetLastError(50);
        return 0;
    }
    const int samples = std::max(1, configValue(EGL_SAMPLES));
    if (format.m_multisampleCount > 1 && format.m_multisampleCount != uint32_t(samples)) {
        SErrSetLastError(50);
        return 0;
    }
    m_format = format;
    m_format.m_size = {width, height};
    m_format.m_multisampleCount = samples;
    m_format.m_window = 1;
    m_format.m_stereoEnabled = false;
    const int depth = configValue(EGL_DEPTH_SIZE), stencil = configValue(EGL_STENCIL_SIZE);
    if (depth < 16) {
        SErrSetLastError(50);
        return 0;
    }
    m_format.m_depthFormat = depth >= 24
                                 ? (stencil >= 8 ? CGxFormat::Fmt_Ds248 : CGxFormat::Fmt_Ds24X)
                                 : CGxFormat::Fmt_Ds160;
    m_format.m_colorFormat =
        configValue(EGL_ALPHA_SIZE) ? CGxFormat::Fmt_Argb8888 : CGxFormat::Fmt_ArgbX888;
    m_defWindowRect = {0, 0, float(height), float(width)};
    if (!m_textureTarget[GxBuffers_Color].m_texture)
        m_curWindowRect = m_defWindowRect;
    backend->gpu.resize(width, height);
    m_viewportDirty = m_scissorRectDirty = 1;
    return 1;
}
void *CGxDeviceGles::DeviceWindow() {
    backend->current();
    return eglGetCurrentSurface(EGL_DRAW);
}
int32_t CGxDeviceGles::DeviceWM(EGxWM message, uintptr_t a, uintptr_t b) {
    backend->current();
    switch (message) {
    case GxWM_Size: {
        require(a <= INT32_MAX && b <= INT32_MAX, "Invalid native Gx resize");
        auto format = m_format;
        format.m_size = {int32_t(a), int32_t(b)};
        return DeviceSetFormat(format);
    }
    case GxWM_SetFocus:
        m_windowFocus = 1;
        return 1;
    case GxWM_KillFocus:
        m_windowFocus = 0;
        return 1;
    case GxWM_Destroy:
        DeviceDestroy();
        return 1;
    case GxWM_DisplayChange: {
        auto format = m_format;
        format.m_size = {0, 0};
        return DeviceSetFormat(format);
    }
    default:
        return 0;
    }
}
void CGxDeviceGles::CapsWindowSize(CRect &rect) {
    backend->current();
    rect = m_curWindowRect;
}
void CGxDeviceGles::CapsWindowSizeInScreenCoords(CRect &rect) {
    // The GL surface fills the Android window: screen and client coordinates coincide.
    backend->current();
    rect = m_curWindowRect;
}
void CGxDeviceGles::SceneClear(uint32_t mask, CImVector color) {
    backend->current();
    backend->target(*this);
    backend->gpu.clear(mask, color);
}
void CGxDeviceGles::ScenePresent() {
    backend->current();
    glFlush();
    if (glGetError() != GL_NO_ERROR)
        throw std::runtime_error("GLES present failed");
    CGxDevice::ScenePresent();
}
void CGxDeviceGles::IRsSendToHw(EGxRenderState which) {
    require(which >= 0 && which < GxRenderStates_Last,
            "Invalid Gx render state"); /* Applied from the synchronized state at
                                           Draw. */
}
char *CGxDeviceGles::BufLock(CGxBuf *buffer) {
    backend->current();
    require(buffer && buffer->m_pool && buffer->m_size, "Invalid Gx buffer lock");
    auto *pool = buffer->m_pool;
    require(!m_bufLocked[pool->m_target], "Gx target is already locked");
    auto &storage = backend->pool(pool);
    backend->reserve(buffer);
    CGxDevice::BufLock(buffer);
    pool->m_mem = storage.bytes.data();
    return reinterpret_cast<char *>(storage.bytes.data() + buffer->m_index);
}
int32_t CGxDeviceGles::BufUnlock(CGxBuf *buffer, uint32_t size) {
    backend->current();
    require(buffer && buffer->m_pool && m_bufLocked[buffer->m_pool->m_target] == buffer,
            "Gx buffer was not locked");
    require(size <= buffer->m_size, "Gx unlock exceeds view");
    auto &storage = backend->pool(buffer->m_pool);
    backend->gpu.updateBuffer(storage.handle, buffer->m_index,
                              storage.bytes.data() + buffer->m_index, size ? size : buffer->m_size);
    CGxDevice::BufUnlock(buffer, size);
    buffer->m_pool->m_mem = nullptr;
    buffer->m_hasData = buffer->m_valid = 1;
    return 1;
}
void CGxDeviceGles::BufData(CGxBuf *buffer, const void *data, uint32_t size, uint32_t offset) {
    backend->current();
    require(buffer && buffer->m_pool && offset <= buffer->m_size &&
                size <= buffer->m_size - offset && (!size || data),
            "Gx buffer write exceeds view");
    require(!m_bufLocked[buffer->m_pool->m_target], "Cannot write a locked Gx target");
    auto &storage = backend->pool(buffer->m_pool);
    if (!buffer->m_hasData)
        backend->reserve(buffer);
    require(buffer->m_index <= storage.bytes.size() &&
                buffer->m_size <= storage.bytes.size() - buffer->m_index,
            "Gx view exceeds pool");
    backend->gpu.updateBuffer(storage.handle, size_t(buffer->m_index) + offset, data, size);
    if (size)
        std::memcpy(storage.bytes.data() + buffer->m_index + offset, data, size);
    CGxDevice::BufData(buffer, data, size, offset);
    buffer->m_hasData = buffer->m_valid = 1;
}
void CGxDeviceGles::PoolSizeSet(CGxPool *pool, uint32_t size) {
    backend->current();
    require(pool && size && size <= INT32_MAX && !m_bufLocked[pool->m_target],
            "Invalid Gx pool resize");
    auto found = backend->pools.find(pool);
    if (found != backend->pools.end()) {
        backend->gpu.destroyBuffer(found->second.handle);
        backend->pools.erase(found);
    }
    pool->m_size = size;
    pool->m_offset = 0;
    pool->m_apiSpecific = nullptr;
    pool->m_mem = nullptr;
    pool->Invalidate();
}
void CGxDeviceGles::PoolDestroy(CGxPool *pool) {
    if (!pool)
        return;
    backend->current();
    require(!m_bufLocked[pool->m_target] && !pool->m_bufList.Head(),
            "Cannot destroy a locked/nonempty Gx pool");
    auto found = backend->pools.find(pool);
    if (found != backend->pools.end()) {
        backend->gpu.destroyBuffer(found->second.handle);
        backend->pools.erase(found);
    }
    pool->m_apiSpecific = nullptr;
    pool->m_mem = nullptr;
    CGxDevice::PoolDestroy(pool);
}
void CGxDeviceGles::TexDestroy(CGxTex *texture) {
    backend->current();
    require(backend->owns(*this, texture), "Invalid native Gx texture");
    if (m_textureTarget[GxBuffers_Color].m_texture == texture)
        DeviceSetRenderTarget(GxBuffers_Color, nullptr, 0);
    for (int which = GxRs_Texture0; which <= GxRs_Texture15; ++which)
        if (m_appRenderStates[which].m_value.m_data.p == texture)
            RsSet(static_cast<EGxRenderState>(which), static_cast<void *>(nullptr));
    for (uint32_t i = 0; i < m_pushedStates.Count(); ++i)
        if (m_pushedStates[i].m_which >= GxRs_Texture0 &&
            m_pushedStates[i].m_which <= GxRs_Texture15 &&
            m_pushedStates[i].m_value.m_data.p == texture)
            m_pushedStates[i].m_value.m_data.p = nullptr;
    auto found = backend->textures.find(texture);
    if (found != backend->textures.end()) {
        backend->gpu.destroyTexture(found->second.handle);
        backend->textures.erase(found);
    }
    texture->m_apiSpecificData = nullptr;
    CGxDevice::TexDestroy(texture);
}
void CGxDeviceGles::ITexMarkAsUpdated(CGxTex *texture) {
    backend->current();
    backend->upload(*this, texture);
}
void CGxDeviceGles::DeviceSetRenderTarget(EGxBuffer buffer, CGxTex *texture, uint32_t face) {
    backend->current();
    if (buffer != GxBuffers_Color)
        unsupported("separate depth texture target");
    if (texture) {
        require(texture->m_flags.m_renderTarget, "Texture is not a Gx render target");
        backend->gpu.setRenderTarget(backend->upload(*this, texture), face);
    } else
        backend->gpu.setRenderTarget();
    CGxDevice::DeviceSetRenderTarget(buffer, texture, face);
    XformSetProjection(m_projection);
    m_viewportDirty = m_scissorRectDirty = 1;
}
void CGxDeviceGles::DeviceReadPixels(CiRect &rect, TSGrowableArray<CImVector> &output) {
    backend->current();
    ClampRectToWindow(rect);
    const int width = rect.maxX - rect.minX, height = rect.maxY - rect.minY;
    if (width <= 0 || height <= 0) {
        output.SetCount(0);
        return;
    }
    backend->target(*this);
    const auto rgba = backend->gpu.readPixels(rect.minX, uint32_t(m_curWindowRect.maxY) - rect.maxY,
                                              width, height);
    output.SetCount(uint32_t(width) * height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto *source = rgba.data() + (size_t(height - 1 - y) * width + x) * 4;
            output[uint32_t(y) * width + x] = CImVector(source[2], source[1], source[0], source[3]);
        }
}
void CGxDeviceGles::Draw(CGxBatch *batches, int32_t count) {
    backend->current();
    require(batches && count > 0, "Invalid Gx batches");
    require(m_primVertexBuf && m_primIndexBuf && m_primVertexBuf->m_pool && m_primIndexBuf->m_pool,
            "Gx draw has no vertex/index streams");
    require(m_primVertexBuf->m_valid && m_primVertexBuf->m_hasData && m_primIndexBuf->m_valid &&
                m_primIndexBuf->m_hasData,
            "Gx draw uses invalidated data");
    auto stateInt = [&](EGxRenderState which) {
        return m_appRenderStates[which].m_value.m_data.i[0];
    };
    for (auto which : {GxRs_VertexShader, GxRs_PixelShader}) {
        auto *shader = static_cast<CGxShader *>(m_appRenderStates[which].m_value.m_data.p);
        if (shader && shader->Valid())
            unsupported("original game shaders");
    }
    if (stateInt(GxRs_ClipPlaneMask))
        unsupported("user clip planes");
    for (int which = GxRs_Texture2; which <= GxRs_Texture15; ++which)
        if (m_appRenderStates[which].m_value.m_data.p)
            unsupported("more than two texture stages");
    if (stateInt(GxRs_TexGen0) || stateInt(GxRs_TexGen1) || stateInt(GxRs_TextureShader0) < 0 ||
        stateInt(GxRs_TextureShader0) > 2 || stateInt(GxRs_TextureShader1) < 0 ||
        stateInt(GxRs_TextureShader1) > 2)
        unsupported("texture coordinate operations/generation");
    IRsSync(0);
    backend->target(*this);
    GPU::DrawState state;
    state.lighting = stateInt(GxRs_Lighting) && MasterEnable(GxMasterEnable_Lighting);
    if (stateInt(GxRs_ColorMaterial) != 0)
        unsupported("emissive/specular color material modes");
    const auto materialState = GxRs_MatDiffuse;
    const uint32_t materialColor = m_appRenderStates[materialState].m_value.m_data.u[0];
    std::memcpy(&state.constantColor, &materialColor, sizeof(materialColor));
    state.fog = stateInt(GxRs_Fog) && MasterEnable(GxMasterEnable_Fog);
    state.fogStart = m_appRenderStates[GxRs_FogStart].m_value.m_data.f[0];
    state.fogEnd = m_appRenderStates[GxRs_FogEnd].m_value.m_data.f[0];
    const uint32_t fogColor = m_appRenderStates[GxRs_FogColor].m_value.m_data.u[0];
    std::memcpy(&state.fogColor, &fogColor, sizeof(fogColor));
    auto textureMatrix = [&](unsigned stage) {
        if (!stateInt(static_cast<EGxRenderState>(GxRs_TextureShader0 + stage)))
            return C44Matrix(1);
        return m_texGen[stage].m_mtx[m_texGen[stage].m_level] *
               m_xforms[stage].m_mtx[m_xforms[stage].m_level];
    };
    state.textureTransform = textureMatrix(0);
    state.secondTextureTransform = textureMatrix(1);
    state.textureCoord = stateInt(GxRs_TextureCoord0);
    state.secondTextureCoord = stateInt(GxRs_TextureCoord1);
    state.secondColorOp = stateInt(GxRs_ColorOp1);
    state.secondAlphaOp = stateInt(GxRs_AlphaOp1);
    state.colorOp = stateInt(GxRs_ColorOp0);
    state.alphaOp = stateInt(GxRs_AlphaOp0);
    state.blend = static_cast<EGxBlend>(stateInt(GxRs_BlendingMode));
    state.alphaRef = std::clamp(stateInt(GxRs_AlphaRef), 0, 255) / 255.0f;
    state.depthTest = stateInt(GxRs_DepthTest) && MasterEnable(GxMasterEnable_DepthTest);
    state.depthWrite = stateInt(GxRs_DepthWrite) && MasterEnable(GxMasterEnable_DepthWrite);
    state.polygonOffset = m_appRenderStates[GxRs_PolygonOffset].m_value.m_data.f[0];
    state.depthFunc = static_cast<EGxCmp>(stateInt(GxRs_DepthFunc));
    state.colorWriteMask =
        MasterEnable(GxMasterEnable_ColorWrite) ? uint32_t(stateInt(GxRs_ColorWrite)) : 0;
    state.cullBackFaces = stateInt(GxRs_Culling) && MasterEnable(GxMasterEnable_Culling);
    state.cullClockwise = stateInt(GxRs_Culling) > 1;
    state.blendFactor = m_appRenderStates[GxRs_BlendFactor].m_value.m_data.f[0];
    const float width = m_curWindowRect.maxX, height = m_curWindowRect.maxY;
    auto round = [](float v) {
        require(std::isfinite(v) && v >= 0 && v < INT32_MAX, "Invalid Gx viewport coordinate");
        return int(v + 0.5f);
    };
    state.scissor = stateInt(GxRs_ScissorTest);
    if (state.scissor) {
        state.x = round(m_scissorRect.minX * width);
        state.y = round(m_scissorRect.minY * height);
        state.width = round(m_scissorRect.maxX * width) - state.x;
        state.height = round(m_scissorRect.maxY * height) - state.y;
    }
    state.viewport = true;
    state.viewportX = round(m_viewport.m_x.m_l * width);
    state.viewportY = round(m_viewport.m_y.m_l * height);
    state.viewportWidth = round(m_viewport.m_x.m_h * width) - state.viewportX;
    state.viewportHeight = round(m_viewport.m_y.m_h * height) - state.viewportY;
    state.minDepth = m_viewport.m_z.m_l;
    state.maxDepth = m_viewport.m_z.m_h;
    C44Matrix modelView = m_xforms[GxXform_World].m_mtx[m_xforms[GxXform_World].m_level] *
                          m_xforms[GxXform_View].m_mtx[m_xforms[GxXform_View].m_level];
    modelView.a2 = -modelView.a2;
    modelView.b2 = -modelView.b2;
    modelView.c2 = -modelView.c2;
    modelView.d2 = -modelView.d2;
    if (state.lighting) {
        state.modelView = modelView;
        state.normalizeNormals = stateInt(GxRs_NormalizeNormals) != 0;
        C33Matrix normal(modelView);
        const float determinant = normal.Determinant();
        require(std::isfinite(determinant) && determinant != 0, "Singular lighting normal matrix");
        const auto inverse = normal.Inverse(determinant);
        const float transposed[9]{inverse.a0, inverse.b0, inverse.c0, inverse.a1, inverse.b1,
                                  inverse.c1, inverse.a2, inverse.b2, inverse.c2};
        std::copy(std::begin(transposed), std::end(transposed), state.normalTransform);
        state.shininess = m_appRenderStates[GxRs_MatSpecularExp].m_value.m_data.f[0];
        const uint32_t specular = m_appRenderStates[GxRs_MatSpecular].m_value.m_data.u[0];
        const uint32_t emissive = m_appRenderStates[GxRs_MatEmissive].m_value.m_data.u[0];
        std::memcpy(&state.specularColor, &specular, sizeof(specular));
        std::memcpy(&state.emissiveColor, &emissive, sizeof(emissive));
        const auto &view = m_xforms[GxXform_View].m_mtx[m_xforms[GxXform_View].m_level];
        for (unsigned i = 0; i < 4; ++i) {
            const auto &source = m_lights[i];
            auto &light = state.lights[i];
            light.enabled = source.m_enable != 0;
            if (!light.enabled)
                continue;
            const float w = source.m_dir.w;
            const float sign = w == 0 ? -1.0f : 1.0f;
            const float x = source.m_dir.x * sign, y = source.m_dir.y * sign,
                        z = source.m_dir.z * sign;
            light.position[0] = x * view.a0 + y * view.b0 + z * view.c0 + w * view.d0;
            light.position[1] = x * view.a1 + y * view.b1 + z * view.c1 + w * view.d1;
            light.position[2] = -(x * view.a2 + y * view.b2 + z * view.c2 + w * view.d2);
            light.position[3] = w;
            const C3Vector colors[3]{source.m_ambColor, source.m_dirColor, source.m_specColor};
            float *out[3]{light.ambient, light.diffuse, light.specular};
            for (unsigned c = 0; c < 3; ++c) {
                out[c][0] = colors[c].x;
                out[c][1] = colors[c].y;
                out[c][2] = colors[c].z;
            }
            light.attenuation[0] = source.m_constantAttenuation;
            light.attenuation[1] = source.m_linearAttenuation;
            light.attenuation[2] = source.m_quadraticAttenuation;
        }
    }
    state.fogDepthTransform[0] = modelView.a2;
    state.fogDepthTransform[1] = modelView.b2;
    state.fogDepthTransform[2] = modelView.c2;
    state.fogDepthTransform[3] = modelView.d2;
    m_projNative = nativeProjection(m_projection, MasterEnable(GxMasterEnable_NormalProjection),
                                    m_textureTarget[GxBuffers_Color].m_texture != nullptr);
    if (m_textureTarget[GxBuffers_Color].m_texture)
        state.cullClockwise = !state.cullClockwise;
    auto *texture = static_cast<CGxTex *>(m_appRenderStates[GxRs_Texture0].m_value.m_data.p);
    const auto sampled = texture ? backend->upload(*this, texture) : GPU::Texture{};
    auto *second = static_cast<CGxTex *>(m_appRenderStates[GxRs_Texture1].m_value.m_data.p);
    state.secondTexture = second ? backend->upload(*this, second) : GPU::Texture{};
    auto &vb = backend->pool(m_primVertexBuf->m_pool);
    auto &ib = backend->pool(m_primIndexBuf->m_pool);
    require(m_primIndexBuf->m_index % 2 == 0 && m_primVertexSize &&
                m_primVertexBuf->m_size % m_primVertexSize == 0,
            "Invalid Gx stream alignment");
    for (int32_t i = 0; i < count; ++i) {
        const auto &batch = batches[i];
        require(batch.m_start <= m_primIndexBuf->m_size / 2 &&
                    batch.m_count <= m_primIndexBuf->m_size / 2 - batch.m_start,
                "Gx batch exceeds index view");
        backend->gpu.drawUnlit(vb.handle, ib.handle, m_primVertexFormat, batch.m_primType,
                               m_primIndexBuf->m_index / 2 + batch.m_start, batch.m_count, sampled,
                               modelView * m_projNative, state, 0, m_primVertexBuf->m_index,
                               m_primVertexBuf->m_size / m_primVertexSize);
    }
    m_viewportDirty = m_scissorRectDirty = m_primVertexDirty = m_primIndexDirty = 0;
    m_hwMasterEnables = m_appMasterEnables;
}
void CGxDeviceGles::XformSetProjection(const C44Matrix &matrix) {
    CGxDevice::XformSetProjection(matrix);
    m_projNative = nativeProjection(matrix, MasterEnable(GxMasterEnable_NormalProjection),
                                    m_textureTarget[GxBuffers_Color].m_texture != nullptr);
}
void CGxDeviceGles::RestoreCurrentContext() {
    backend->gpu.loseContext();
    backend->gpu.attachCurrentContext();
    backend->context = eglGetCurrentContext();
    backend->thread = std::this_thread::get_id();
    backend->attached = true;
    m_context = 1;
    auto format = m_format;
    format.m_size = {0, 0};
    if (!DeviceSetFormat(format))
        throw std::runtime_error("Cannot restore Gx surface format");
    std::vector<CGxTex *> snapshot;
    for (auto *texture = m_textures.Head(); texture; texture = m_textures.Next(texture))
        snapshot.push_back(texture);
    for (auto *texture : snapshot)
        if (backend->owns(*this, texture)) {
            texture->m_needsCreation = texture->m_needsUpdate = 1;
            if (texture->m_userFunc) {
                uint32_t stride = 0;
                const void *pixels = nullptr;
                texture->m_userFunc(GxTex_Recreate, texture->m_width, texture->m_height, 0, 0,
                                    texture->m_userArg, stride, pixels);
            }
        }
    IRsForceUpdate();
    NotifyOnTextureRecreation();
    NotifyOnDeviceRestored();
}
void CGxDeviceGles::DeviceEvictResources() { unsupported("selective resource eviction"); }
void CGxDeviceGles::TexSetCacheSize(uint32_t) { unsupported("texture cache budget"); }
void CGxDeviceGles::DeviceReadDepths(CiRect &, TSGrowableArray<float> &) {
    unsupported("depth readback");
}
int32_t CGxDeviceGles::TexCopy(CGxTex *, const C2iVector &, const C2iVector &, const C2iVector &,
                               uint32_t, uint32_t) {
    unsupported("framebuffer texture copy");
}
int32_t CGxDeviceGles::TexStretch(CGxTex *, const CiRect *, const CiRect *, uint32_t, uint32_t) {
    unsupported("stretched texture copy");
}
bool CGxDeviceGles::QueryBegin(CGxQuery *) { unsupported("occlusion queries"); }
bool CGxDeviceGles::QueryEnd(CGxQuery *) { unsupported("occlusion queries"); }
bool CGxDeviceGles::QueryGetParam(CGxQuery *, EGxQueryParam, uint32_t &) {
    unsupported("occlusion queries");
}
bool CGxDeviceGles::QueryGetData(CGxQuery *, uint32_t &) { unsupported("occlusion queries"); }
void CGxDeviceGles::IShaderReload(CGxShader *, const char *, const char *) {
    unsupported("game shader reload");
}
void CGxDeviceGles::IShaderCreate(CGxShader *shader) {
    // Game shaders are not translated: callers (fonts, CSimpleRender, models) check
    // Valid() and take their fixed-function path.
    if (shader) {
        shader->m_loaded = 1;
        shader->m_valid = 0;
    }
}
void CGxDeviceGles::StereoSetConvergence(float value) {
    if (value)
        unsupported("stereo convergence");
}
float CGxDeviceGles::StereoGetConvergence() { return 0; }
void CGxDeviceGles::StereoSetSeparation(float value) {
    if (value)
        unsupported("stereo separation");
}
float CGxDeviceGles::StereoGetSeparation() { return 0; }
bool CGxDeviceGles::StereoEnabled() { return false; }
void CGxDeviceGles::DeviceUnk81(uint32_t, uint32_t) { unsupported("device operation 81"); }
void CGxDeviceGles::DeviceUnk82() { unsupported("device operation 82"); }
void CGxDeviceGles::DeviceUnk83(uint32_t, uint32_t) { unsupported("device operation 83"); }
void CGxDeviceGles::DeviceCopyTex(CGxTex *, uint32_t, CGxTex *, uint32_t) {
    unsupported("texture copy");
}
void CGxDeviceGles::DeviceSaveDepthSurface() { unsupported("depth surface save"); }
void CGxDeviceGles::DeviceRestoreDepthSurface() { unsupported("depth surface restore"); }
void CGxDeviceGles::DeviceResolveDepthBuffer(CGxTex *) { unsupported("depth buffer resolve"); }
void CGxDeviceGles::PrimPointSize(float size) {
    backend->current();
    if (size != 1)
        unsupported("point size");
}
void CGxDeviceGles::PrimLineWidth(float size) {
    backend->current();
    if (size != 1)
        unsupported("wide lines");
    glLineWidth(size);
}
void CGxDeviceGles::DeviceSetGamma(float gamma) {
    if (gamma != 1)
        unsupported("display gamma");
    CGxDevice::DeviceSetGamma(gamma);
}
void CGxDeviceGles::DeviceSetGamma(const CGxGammaRamp &ramp) {
    CGxGammaRamp linear;
    linear.Set(1);
    if (std::memcmp(&linear, &ramp, sizeof(ramp)))
        unsupported("display gamma ramp");
    CGxDevice::DeviceSetGamma(ramp);
}
CGxDevice *CreateCGxDeviceOpenGl() { return new CGxDeviceGles; }
int32_t CGxDevice::DeviceAdapterID(uint16_t &vendor, uint16_t &device, uint32_t &hi, uint32_t &lo) {
    vendor = device = 0;
    hi = lo = 0;
    SErrSetLastError(50);
    return 0;
}
int32_t CGxDevice::DeviceAdapterInfer(uint16_t &device) {
    device = 0;
    SErrSetLastError(50);
    return 0;
}
int32_t CGxDevice::AdapterDesktopMode(CGxMonitorMode &mode) {
    int w = 0, h = 0;
    if (!surfaceSize(w, h)) {
        SErrSetLastError(6);
        return 0;
    }
    mode.m_size = {w, h};
    mode.m_bpp = configValue(EGL_RED_SIZE) + configValue(EGL_GREEN_SIZE) +
                 configValue(EGL_BLUE_SIZE) + configValue(EGL_ALPHA_SIZE);
    mode.m_refreshRate = 0;
    return 1;
}
int32_t CGxDevice::AdapterMonitorModes(TSGrowableArray<CGxMonitorMode> &modes) {
    modes.SetCount(0);
    CGxMonitorMode mode;
    if (!AdapterDesktopMode(mode))
        return 0;
    *modes.New() = mode;
    return 1;
}
int32_t CGxDevice::OpenGlAdapterFormats(TSGrowableArray<CGxFormat> &formats) {
    formats.SetCount(0);
    CGxMonitorMode mode;
    if (!AdapterDesktopMode(mode))
        return 0;
    CGxFormat format(
        true, mode.m_size,
        configValue(EGL_ALPHA_SIZE) ? CGxFormat::Fmt_Argb8888 : CGxFormat::Fmt_ArgbX888,
        configValue(EGL_DEPTH_SIZE) >= 24
            ? (configValue(EGL_STENCIL_SIZE) >= 8 ? CGxFormat::Fmt_Ds248 : CGxFormat::Fmt_Ds24X)
            : CGxFormat::Fmt_Ds160,
        0, 1, true, false, false, true, 0);
    *formats.New() = format;
    return 1;
}
int32_t CGxDevice::AdapterFormats(EGxApi api, TSGrowableArray<CGxFormat> &formats) {
    formats.SetCount(0);
    if (api != GxApi_OpenGl) {
        SErrSetLastError(50);
        return 0;
    }
    return OpenGlAdapterFormats(formats);
}
