#include "M2PreviewTextures.h"
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
                "Invalid M2 preview texture request");
    stride = width * 4;
    texels = image.rgba.data();
}
} // namespace
M2PreviewTextures::M2PreviewTextures(std::shared_ptr<const M2PreviewMaterials> materials)
    : materials_(std::move(materials)),
      textures_(materials_ ? materials_->textures.size() : 0, nullptr) {}
M2PreviewTextures::~M2PreviewTextures() {
    for (auto *texture : textures_)
        if (texture)
            GxTexDestroy(texture);
}
CGxTex *M2PreviewTextures::get(size_t index) {
    m2::require(index < textures_.size(), "Invalid M2 preview GPU texture index");
    if (!textures_[index]) {
        const auto &texture = materials_->textures[index];
        m2::require(bool(texture.pixels), "Missing M2 preview pixels");
        const auto &image = *texture.pixels;
        CGxTex *created = nullptr;
        try {
            // Power-of-two textures get a generated mip chain (trilinear): without it the
            // distant scene geometry shimmers and looks noisy on phone screens.
            const auto pow2 = [](uint32_t x) { return x && !(x & (x - 1)); };
            const bool mips = pow2(image.width) && pow2(image.height);
            if (!GxTexCreate(image.width, image.height, GxTex_Abgr8888,
                             CGxTexFlags(mips ? GxTex_LinearMipLinear : GxTex_Linear,
                                         texture.flags & 1, (texture.flags >> 1) & 1, 0, mips),
                             const_cast<TexturePixels *>(&image), source, created))
                throw std::runtime_error("M2 preview Gx texture creation failed");
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
