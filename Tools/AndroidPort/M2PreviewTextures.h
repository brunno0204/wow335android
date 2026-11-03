#pragma once
#include "M2PreviewMaterials.h"
class CGxTex;
namespace wow::android {
// GL-thread-only lifetime; destroy before the Gx device. CPU assets may be
// loaded on a worker. Logical Gx textures restore after EGL replacement.
class M2PreviewTextures {
  public:
    explicit M2PreviewTextures(std::shared_ptr<const M2PreviewMaterials> materials);
    ~M2PreviewTextures();
    M2PreviewTextures(const M2PreviewTextures &) = delete;
    M2PreviewTextures &operator=(const M2PreviewTextures &) = delete;
    CGxTex *get(size_t texture);
    bool matches(const std::shared_ptr<const M2PreviewMaterials> &data) const noexcept {
        return data == materials_;
    }

  private:
    std::shared_ptr<const M2PreviewMaterials> materials_;
    std::vector<CGxTex *> textures_;
};
} // namespace wow::android
