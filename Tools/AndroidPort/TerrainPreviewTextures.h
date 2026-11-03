#pragma once
#include "TerrainMaterials.h"
class CGxTex;
namespace wow::android {
// GL/frame owner only. Retains CPU pixels for logical texture EGL restoration.
class TerrainPreviewTextures {
  public:
    explicit TerrainPreviewTextures(std::shared_ptr<const TerrainPreviewMaterials> materials);
    ~TerrainPreviewTextures();
    TerrainPreviewTextures(const TerrainPreviewTextures &) = delete;
    TerrainPreviewTextures &operator=(const TerrainPreviewTextures &) = delete;
    CGxTex *get(size_t index);
    bool matches(const std::shared_ptr<const TerrainPreviewMaterials> &materials) const noexcept {
        return materials == materials_;
    }

  private:
    std::shared_ptr<const TerrainPreviewMaterials> materials_;
    std::vector<CGxTex *> textures_;
};
} // namespace wow::android
