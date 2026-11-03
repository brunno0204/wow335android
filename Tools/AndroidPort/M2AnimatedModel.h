#pragma once
#include "M2AnimationPlayer.h"
#include "M2Geometry.h"
#include "M2PreviewMaterials.h"
#include <memory>
#include <optional>
namespace wow::android {
struct M2AnimatedModelData;
struct M2ModelSequence {
    uint32_t index = 0, duration = 0;
    uint16_t id = 0, variation = 0;
};
// Frame-thread-owned geometry + preloaded clips + playback. Copies share
// immutable assets but have independent clocks. No file/archive access during
// play/sample. This is the checked basic M2 subset, not CM2Model/materials or a
// character controller.
class M2AnimatedModel {
  public:
    // One MD20 snapshot supplies geometry AND all requested sequence slots.
    // 1..32 distinct slots, first starts immediately. External resources are
    // shared within this load and bounded to 128 MiB combined. Optional preview
    // material preload uses these same snapshots and finishes before returning;
    // it is the basic development subset, not full M2 material evaluation.
    static M2AnimatedModel load(const std::string &modelPath, const std::string &skinPath,
                                const std::vector<uint32_t> &sequenceIndices = {0},
                                bool preloadPreviewMaterials = false,
                                const M2TextureReplacements &replacements = {});
    const M2Geometry &geometry() const noexcept;
    std::shared_ptr<const M2PreviewMaterials> previewMaterials() const noexcept;
    std::shared_ptr<const M2PreviewMaterialSource> materialSource() const noexcept;
    // Preparation reads BLPs through current SFile mounts and touches only
    // immutable recipes. Use a loading-thread snapshot, not a moving instance.
    std::shared_ptr<const M2PreviewMaterials>
    preparePreviewMaterials(const M2TextureReplacements &replacements) const;
    // Frame thread only: no IO, rejects foreign recipes before changing state.
    void installPreviewMaterials(std::shared_ptr<const M2PreviewMaterials> materials);
    const std::vector<M2ModelSequence> &sequences() const noexcept;
    const M2AnimationPlayer &playback() const noexcept { return player_; }
    uint32_t sequence() const noexcept { return sequence_; }
    // Exact preloaded slot, no fallback/random variation or synchronous IO.
    // Failed selection preserves the previous slot and playback state.
    void play(uint32_t sequenceIndex, const M2PlaybackOptions &options = {});
    // Exact requested ID/variation, including alias slots. Missing/ambiguous
    // pairs reject without changing playback; DBC fallbacks remain separate.
    void playAnimation(uint16_t id, uint16_t variation = 0, const M2PlaybackOptions &options = {});
    void advance(uint64_t milliseconds) { player_.advance(milliseconds); }
    void pause(bool value) noexcept { player_.pause(value); }
    void seek(uint64_t milliseconds) { player_.seek(milliseconds); }
    void setSpeed(double speed) { player_.setSpeed(speed); }
    void setLoop(bool loop) { player_.setLoop(loop); }
    // Output has the SKIN lookup's vertex numbering. Only section-used vertices
    // are transformed (once, even across overlapping sections); unused entries
    // are zero. Apply world here OR at draw, never both.
    std::vector<M2VertexPNT2> sample(const C44Matrix &world = C44Matrix(1)) const;
    // Current pose of an M2Attachment (by attachment ID) composed with world.
    std::optional<C44Matrix> attachment(uint32_t id, const C44Matrix &world = C44Matrix(1)) const;
    bool hasAttachment(uint32_t id) const noexcept;
    // Current bone matrices composed with world (particle emitters).
    std::vector<C44Matrix> bones(const C44Matrix &world = C44Matrix(1)) const { return player_.evaluate(world); }

  private:
    explicit M2AnimatedModel(std::shared_ptr<const M2AnimatedModelData> data, uint32_t first,
                             std::shared_ptr<const M2PreviewMaterials> materials);
    std::shared_ptr<const M2AnimatedModelData> data_;
    std::shared_ptr<const M2PreviewMaterials> materials_;
    M2AnimationPlayer player_;
    uint32_t sequence_ = 0;
};
} // namespace wow::android
