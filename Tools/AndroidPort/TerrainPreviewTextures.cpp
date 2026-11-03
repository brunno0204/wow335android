#include "TerrainPreviewTextures.h"
#include "Engine/Source/Gx/Gx.h"
#include "M2File.h"
namespace wow::android {
namespace {
void source(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t face, uint32_t level,
            void *user, uint32_t &stride, const void *&texels) {
    if (command != GxTex_Latch)
        return;
    const auto &image = *static_cast<const TexturePixels *>(user);
    m2::require(width == image.width && height == image.height && !face && !level,
                "Invalid terrain texture latch");
    stride = width * 4;
    texels = image.rgba.data();
}
} // namespace
TerrainPreviewTextures::TerrainPreviewTextures(
    std::shared_ptr<const TerrainPreviewMaterials> materials)
    : materials_(std::move(materials)),
      textures_(materials_ ? materials_->textures.size() : 0, nullptr) {}
TerrainPreviewTextures::~TerrainPreviewTextures() {
    for (auto *t : textures_)
        if (t)
            GxTexDestroy(t);
}
CGxTex *TerrainPreviewTextures::get(size_t index) {
    m2::require(index < textures_.size() && materials_->textures[index],
                "Invalid terrain texture slot");
    if (!textures_[index]) {
        const auto &image = *materials_->textures[index];
        const bool repeat = materials_->clamp.empty() || !materials_->clamp[index];
        CGxTex *created = nullptr;
        try {
            if (!GxTexCreate(image.width, image.height, GxTex_Abgr8888,
                             CGxTexFlags(image.width && !(image.width & (image.width - 1)) &&
                                                 image.height && !(image.height & (image.height - 1))
                                             ? GxTex_LinearMipLinear : GxTex_Linear,
                                         repeat, repeat, 0,
                                         image.width && !(image.width & (image.width - 1)) &&
                                             image.height && !(image.height & (image.height - 1))),
                             const_cast<TexturePixels *>(&image), source, created))
                throw std::runtime_error("Terrain Gx texture creation failed");
        } catch (...) {
            if (created)
                GxTexDestroy(created);
            throw;
        }
        textures_[index] = created;
    }
    return textures_[index];
}
} // namespace wow::android
