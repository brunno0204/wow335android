#include "M2ModelPreview.h"
#include "ClientData.h"
#include "M2File.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace wow::android {
M2ModelPreview::M2ModelPreview(M2AnimatedModel model) : model_(std::move(model)) {
    const auto vertices = model_.sample();
    double low[3]{INFINITY, INFINITY, INFINITY}, high[3]{-INFINITY, -INFINITY, -INFINITY};
    size_t triangles = 0;
    // Fit actually indexed section geometry. Unused lookup slots do not affect
    // framing. Repeated sections/indices do not change extrema.
    const auto &geometry = model_.geometry();
    geosets_ = prepareM2Geosets(makeM2GeosetCatalog(geometry), std::nullopt);
    sequences_ = std::make_shared<const M2PreviewSequenceCatalog>(model_.sequences());
    for (const auto &section : geometry.sections)
        for (uint32_t i = section.m_indexStart; i < section.m_indexStart + section.m_indexCount;
             ++i) {
            const auto &p = vertices[geometry.indices[i]].m_position;
            const double value[3]{p.x, p.y, p.z};
            for (unsigned k = 0; k < 3; ++k) {
                m2::require(std::isfinite(value[k]), "Nonfinite M2 preview bounds");
                low[k] = std::min(low[k], value[k]);
                high[k] = std::max(high[k], value[k]);
            }
            ++triangles;
        }
    m2::require(triangles > 0, "M2 preview has no section triangles");
    double squared = 0;
    for (unsigned k = 0; k < 3; ++k) {
        center_[k] = (low[k] + high[k]) * .5;
        const double extent = (high[k] - low[k]) * .5;
        squared += extent * extent;
    }
    radius_ = std::sqrt(squared);
    m2::require(std::isfinite(radius_) && radius_ >= 1e-6, "Degenerate M2 preview bounds");
}
M2ModelPreview M2ModelPreview::loadInstalled(const std::string &dataRoot,
                                             const std::string &modelPath,
                                             const std::string &skinPath, uint32_t sequenceIndex,
                                             const M2TextureReplacements &replacements) {
    return loadInstalledSequences(dataRoot, modelPath, skinPath, {sequenceIndex}, replacements);
}
M2ModelPreview M2ModelPreview::loadInstalledSequences(const std::string &dataRoot,
                                                      const std::string &modelPath,
                                                      const std::string &skinPath,
                                                      const std::vector<uint32_t> &sequenceIndices,
                                                      const M2TextureReplacements &replacements) {
    const auto mount = sharedClientData(dataRoot);
        const auto &install = *mount;
    if (!install.report().ready)
        throw std::runtime_error(
            "Local game data is incomplete; inspect Data before loading a model");
    return M2ModelPreview(
        M2AnimatedModel::load(modelPath, skinPath, sequenceIndices, true, replacements));
}
std::shared_ptr<const M2PreviewMaterials>
M2ModelPreview::prepareInstalledAppearance(const std::string &dataRoot,
                                           std::shared_ptr<const M2PreviewMaterialSource> source,
                                           const M2TextureReplacements &replacements) {
    m2::require(bool(source), "No active model material recipe");
    const auto mount = sharedClientData(dataRoot);
        const auto &install = *mount;
    if (!install.report().ready)
        throw std::runtime_error("Local game data is incomplete");
    return std::make_shared<M2PreviewMaterials>(
        prepareM2PreviewMaterials(std::move(source), replacements));
}
void M2ModelPreview::installGeosets(std::shared_ptr<const M2GeosetSelection> selection) {
    m2::require(selection && selection->source == geosetCatalog() &&
                    selection->visible.size() == model_.geometry().sections.size(),
                "Foreign or invalid M2 geoset selection");
    for (uint8_t visible : selection->visible)
        m2::require(visible <= 1, "Invalid M2 geoset visibility");
    geosets_ = std::move(selection);
}
void M2ModelPreview::frame(uint64_t milliseconds) {
    const auto delta = lastFrame_ && milliseconds >= *lastFrame_
                           ? std::min<uint64_t>(milliseconds - *lastFrame_, 100)
                           : 0;
    model_.advance(delta);
    lastFrame_ = milliseconds;
}
void M2ModelPreview::controlPlayback(const M2PreviewPlaybackRequest &request) {
    m2::require(request.source == sequences_, "Foreign M2 preview playback request");
    const auto checked = prepareM2PreviewPlayback(sequences_, request);
    switch (checked.action) {
    case M2PreviewPlaybackAction::Play:
        model_.play(checked.sequence, checked.options);
        break;
    case M2PreviewPlaybackAction::Pause:
        model_.pause(checked.enabled);
        break;
    case M2PreviewPlaybackAction::Seek:
        model_.seek(checked.milliseconds);
        break;
    case M2PreviewPlaybackAction::Speed:
        model_.setSpeed(checked.speed);
        break;
    case M2PreviewPlaybackAction::Loop:
        model_.setLoop(checked.enabled);
        break;
    }
}
void M2ModelPreview::orbit(float horizontalPixels, float verticalPixels) {
    m2::require(std::isfinite(horizontalPixels) && std::isfinite(verticalPixels),
                "Invalid preview orbit");
    constexpr double pi = 3.14159265358979323846;
    // Reduce each finite input in double precision before accumulating it.
    yaw_ = std::remainder(yaw_ + std::remainder(double(horizontalPixels) * .005, 2 * pi), 2 * pi);
    pitch_ = std::clamp(pitch_ + double(verticalPixels) * .005, -1.4, 1.4);
}
C44Matrix M2ModelPreview::projection(float aspect) const {
    m2::require(std::isfinite(aspect) && aspect > 0, "Invalid preview aspect");
    const double sx = .85 / radius_ / std::max(1.0, double(aspect));
    const double sy = .85 / radius_ * std::min(1.0, double(aspect));
    const double sz = .5 / radius_;
    const double c = std::cos(yaw_), s = std::sin(yaw_), cp = std::cos(pitch_),
                 sp = std::sin(pitch_);
    C44Matrix m(1);
    // WoW Z-up -> screen up. Orthographic orbit, centered on the initial pose.
    m.a0 = float(c * sx);
    m.b0 = float(-s * sx);
    m.c0 = 0;
    m.a1 = float(s * sp * sy);
    m.b1 = float(c * sp * sy);
    m.c1 = float(cp * sy);
    m.a2 = float(s * cp * sz);
    m.b2 = float(c * cp * sz);
    m.c2 = float(-sp * sz);
    m.d0 = float(-center_[0] * m.a0 - center_[1] * m.b0 - center_[2] * m.c0);
    m.d1 = float(-center_[0] * m.a1 - center_[1] * m.b1 - center_[2] * m.c1);
    m.d2 = float(-center_[0] * m.a2 - center_[1] * m.b2 - center_[2] * m.c2);
    const float values[]{m.a0, m.b0, m.c0, m.d0, m.a1, m.b1, m.c1, m.d1, m.a2, m.b2, m.c2, m.d2};
    for (float value : values)
        m2::require(std::isfinite(value), "M2 preview projection overflow");
    return m;
}
} // namespace wow::android
