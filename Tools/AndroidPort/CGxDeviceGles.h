#pragma once
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include <memory>

// An original CGxDevice implementation for an externally owned EGL context.
// Unsupported material/device features fail explicitly; they are never rendered
// through an unlit substitute. The fixed unlit Gx path is implemented first.
class CGxDeviceGles final : public CGxDevice {
  public:
    CGxDeviceGles();
    ~CGxDeviceGles() override;
    int32_t DeviceCreate(GxWindowProc callback, const CGxFormat &format) override;
    int32_t DeviceCreate(void *surface, const CGxFormat &format) override;
    void DeviceDestroy() override;
    void DeviceEvictResources() override;
    void TexSetCacheSize(uint32_t size) override;
    int32_t DeviceSetFormat(const CGxFormat &format) override;
    void *DeviceWindow() override;
    int32_t DeviceWM(EGxWM message, uintptr_t a, uintptr_t b) override;
    void CapsWindowSize(CRect &rect) override;
    void CapsWindowSizeInScreenCoords(CRect &rect) override;
    void SceneClear(uint32_t mask, CImVector color) override;
    void ScenePresent() override;
    void Draw(CGxBatch *batches, int32_t count) override;
    void XformSetProjection(const C44Matrix &matrix) override;
    void IRsSendToHw(EGxRenderState which) override;
    char *BufLock(CGxBuf *buffer) override;
    int32_t BufUnlock(CGxBuf *buffer, uint32_t size) override;
    void BufData(CGxBuf *buffer, const void *data, uint32_t size, uint32_t offset) override;
    void PoolSizeSet(CGxPool *pool, uint32_t size) override;
    void PoolDestroy(CGxPool *pool) override;
    void TexDestroy(CGxTex *texture) override;
    void ITexMarkAsUpdated(CGxTex *texture) override;
    void DeviceSetRenderTarget(EGxBuffer buffer, CGxTex *texture, uint32_t plane) override;
    void DeviceReadPixels(CiRect &rect, TSGrowableArray<CImVector> &pixels) override;
    void DeviceReadDepths(CiRect &, TSGrowableArray<float> &) override;
    int32_t TexCopy(CGxTex *, const C2iVector &, const C2iVector &, const C2iVector &, uint32_t,
                    uint32_t) override;
    int32_t TexStretch(CGxTex *, const CiRect *, const CiRect *, uint32_t, uint32_t) override;
    bool QueryBegin(CGxQuery *) override;
    bool QueryEnd(CGxQuery *) override;
    bool QueryGetParam(CGxQuery *, EGxQueryParam, uint32_t &) override;
    bool QueryGetData(CGxQuery *, uint32_t &) override;
    void IShaderReload(CGxShader *, const char *, const char *) override;
    void IShaderCreate(CGxShader *) override;
    void StereoSetConvergence(float) override;
    float StereoGetConvergence() override;
    void StereoSetSeparation(float) override;
    float StereoGetSeparation() override;
    bool StereoEnabled() override;
    void DeviceUnk81(uint32_t, uint32_t) override;
    void DeviceUnk82() override;
    void DeviceUnk83(uint32_t, uint32_t) override;
    void DeviceCopyTex(CGxTex *, uint32_t, CGxTex *, uint32_t) override;
    void DeviceSaveDepthSurface() override;
    void DeviceRestoreDepthSurface() override;
    void DeviceResolveDepthBuffer(CGxTex *) override;
    void PrimPointSize(float) override;
    void PrimLineWidth(float) override;
    void DeviceSetGamma(float) override;
    void DeviceSetGamma(const CGxGammaRamp &) override;
    // Called only after the host replaced/destroyed the old context.
    void RestoreCurrentContext();

  private:
    struct Backend;
    std::unique_ptr<Backend> backend;
};
