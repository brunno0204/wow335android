#pragma once
#include "M2AnimatedModel.h"
#include <deque>
namespace wow::android {
using M2PreviewSequenceCatalog = std::vector<M2ModelSequence>;
enum class M2PreviewPlaybackAction { Play, Pause, Seek, Speed, Loop };
struct M2PreviewPlaybackRequest {
    std::shared_ptr<const M2PreviewSequenceCatalog> source;
    M2PreviewPlaybackAction action = M2PreviewPlaybackAction::Play;
    uint32_t sequence = 0;
    M2PlaybackOptions options;
    uint64_t milliseconds = 0;
    double speed = 1;
    bool enabled = false;
};
// 1..32 distinct decimal slots; first requested slot starts. No wildcard/fallback.
std::vector<uint32_t> parseM2PreviewSequences(const std::string &text);
// Checks immutable metadata only. Clip transition preflight remains on frame owner.
M2PreviewPlaybackRequest
prepareM2PreviewPlayback(std::shared_ptr<const M2PreviewSequenceCatalog> source,
                         M2PreviewPlaybackRequest request);
// External handoff mutex required. FIFO preserves play/pause/seek order. No file or GL access.
class M2PreviewPlaybackQueue {
  public:
    void push(M2PreviewPlaybackRequest request);
    std::deque<M2PreviewPlaybackRequest> take();
    void clear() noexcept { requests_.clear(); }

  private:
    std::deque<M2PreviewPlaybackRequest> requests_;
};
} // namespace wow::android
