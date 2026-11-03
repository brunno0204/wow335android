#pragma once
#include "GlueScene.h"
#include "CharacterCustomization.h"
#include <optional>
#include <string>
#include "M2ModelPreview.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
namespace wow::android {
// Owned Glue scene published by the asset worker and adopted by the GL thread.
// An empty background clears the active scene.
struct GlueSceneData {
    std::unique_ptr<M2ModelPreview> background, character;
    // Equipment models on the character's M2 attachment points (helm, shoulders, weapons).
    std::vector<std::pair<std::unique_ptr<M2ModelPreview>, uint32_t>> items;
    M2SceneInfo info;
    // Background particles (created on the GL thread from the background's effects).
    std::unique_ptr<M2ParticleRuntime> particles;
    bool resetFacing = false;
    uint32_t serial = 0;
};
std::mutex &glueSceneMutex();
std::unique_ptr<GlueSceneData> &pendingGlueScene(); // guarded by glueSceneMutex
std::atomic<float> &glueFacing();
// Asset worker: full appearance (CharSections textures, geosets) with stand/run clips.
// Asset worker: original character rules (DBC-backed), loaded on first use for root.
const CharacterCustomization &characterRules(const std::string &root);
std::unique_ptr<M2ModelPreview> loadWorldCharacter(const std::string &root, const CharacterLook &look,
                                                   std::optional<uint32_t> &stand,
                                                   std::optional<uint32_t> &run, std::string &note);
} // namespace wow::android
