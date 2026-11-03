#pragma once
#include "M2AnimatedModel.h"
#include "M2Effects.h"
#include "M2Geosets.h"
#include "M2PreviewPlayback.h"
#include <optional>
namespace wow::android {
class M2PreviewTextures;
class GxStaticMesh;
// Development-only, basic textured preview of the supported M2 subset. Owned by
// the GL/frame thread after loading. It is not game/character startup or full
// original materials.
class M2ModelPreview {
  public:
    explicit M2ModelPreview(M2AnimatedModel model);
    // Synchronously mounts the local installation, loads one sequence and
    // closes all mounts before returning. Call on the asset loading thread.
    static M2ModelPreview loadInstalled(const std::string &dataRoot, const std::string &modelPath,
                                        const std::string &skinPath, uint32_t sequenceIndex,
                                        const M2TextureReplacements &replacements = {});
    static M2ModelPreview loadInstalledSequences(const std::string &dataRoot,
                                                 const std::string &modelPath,
                                                 const std::string &skinPath,
                                                 const std::vector<uint32_t> &sequenceIndices,
                                                 const M2TextureReplacements &replacements = {});
    static std::shared_ptr<const M2PreviewMaterials>
    prepareInstalledAppearance(const std::string &dataRoot,
                               std::shared_ptr<const M2PreviewMaterialSource> source,
                               const M2TextureReplacements &replacements);
    void installAppearance(std::shared_ptr<const M2PreviewMaterials> appearance) {
        model_.installPreviewMaterials(std::move(appearance));
    }
    void resetFrameClock() noexcept { lastFrame_.reset(); }
    // Boot-monotonic milliseconds. First/backward frame has zero delta; stalls
    // are capped at 100 ms. Explicit reset prevents catch-up after suspend.
    void frame(uint64_t milliseconds);
    void orbit(float horizontalPixels, float verticalPixels);
    C44Matrix projection(float aspect) const;
    void
    draw(float aspect,
         M2PreviewTextures *textures = nullptr) const; // Current original Gx/GLES device required.
    // Scene-owned rigid placement and camera; model sampling remains in local space.
    void drawPlaced(const C44Matrix &world, const C44Matrix &view,
                    const C44Matrix &projection, M2PreviewTextures *textures = nullptr) const;
    // Same pose for many placements (map doodads); one skinning sample per call.
    void drawInstances(const std::vector<C44Matrix> &worlds, const C44Matrix &view,
                       const C44Matrix &projection, M2PreviewTextures *textures = nullptr) const;
    // GL thread: instances with a caller-owned GPU vertex cache (static poses skinned once,
    // animated ones at most every resampleMs).
    void drawCached(GxStaticMesh &cache, const std::vector<C44Matrix> &worlds, const C44Matrix &view,
                    const C44Matrix &projection, M2PreviewTextures *textures, uint64_t nowMs,
                    uint32_t resampleMs) const;
    const M2AnimatedModel &model() const noexcept { return model_; }
    std::shared_ptr<const M2GeosetCatalog> geosetCatalog() const noexcept {
        return geosets_->source;
    }
    std::shared_ptr<const M2GeosetSelection> geosets() const noexcept { return geosets_; }
    // Frame thread only. No IO, refit, texture-cache replacement or clock reset.
    void installGeosets(std::shared_ptr<const M2GeosetSelection> selection);
    std::shared_ptr<const M2PreviewSequenceCatalog> sequenceCatalog() const noexcept {
        return sequences_;
    }
    // Frame owner only. Preserves geometry, appearance, visibility and fitted camera.
    void controlPlayback(const M2PreviewPlaybackRequest &request);
    // Frame owner: play a preloaded sequence slot (no-op if it is already playing).
    void play(uint32_t slot, bool loop = true) {
        if (model_.sequence() == slot && loop)
            return;
        M2PlaybackOptions options;
        options.loop = loop;
        model_.play(slot, options);
    }
    M2AnimatedModel &animated() noexcept { return model_; }
    // Animated colors/weights/texture transforms of the playing sequence (frame thread).
    void setEffects(std::shared_ptr<const M2EffectsData> effects) { effects_ = std::move(effects); }
    const std::shared_ptr<const M2EffectsData> &effects() const noexcept { return effects_; }
    const double *boundsCenter() const noexcept { return center_; }
    double boundsRadius() const noexcept { return radius_; }

  private:
    void drawTransforms(const C44Matrix &world, const C44Matrix &view,
                        const C44Matrix &projection, M2PreviewTextures *textures,
                        bool normalProjection) const;
    void drawMany(const std::vector<C44Matrix> &worlds, const C44Matrix &view,
                  const C44Matrix &projection, M2PreviewTextures *textures,
                  bool normalProjection, GxStaticMesh *cache = nullptr, uint64_t nowMs = 0,
                  uint32_t resampleMs = 0) const;
    M2AnimatedModel model_;
    std::shared_ptr<const M2GeosetSelection> geosets_;
    std::shared_ptr<const M2PreviewSequenceCatalog> sequences_;
    double center_[3]{}, radius_ = 1;
    double yaw_ = .4, pitch_ = .35;
    std::optional<uint64_t> lastFrame_;
    std::shared_ptr<const M2EffectsData> effects_;
};
} // namespace wow::android
