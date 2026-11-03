#include "M2PreviewPlayback.h"
#include "M2File.h"
#include <algorithm>
#include <cmath>
#include <string_view>
namespace wow::android {
std::vector<uint32_t> parseM2PreviewSequences(const std::string &text) {
    m2::require(text.size() <= 1024, "M2 sequence list too long");
    std::string_view remaining(text);
    std::vector<uint32_t> result;
    while (true) {
        const auto comma = remaining.find(',');
        auto token = remaining.substr(0, comma);
        const auto begin = token.find_first_not_of(" \t\r\n");
        m2::require(begin != token.npos, "Empty M2 sequence slot");
        token = token.substr(begin, token.find_last_not_of(" \t\r\n") - begin + 1);
        m2::require(token.size() <= 10, "Invalid M2 sequence slot");
        uint64_t value = 0;
        for (char c : token) {
            m2::require(c >= '0' && c <= '9', "Invalid M2 sequence decimal slot");
            value = value * 10 + unsigned(c - '0');
        }
        m2::require(value < 4096, "M2 sequence slot exceeds decoder limit");
        m2::require(result.size() < 32, "Too many M2 sequence slots");
        m2::require(std::find(result.begin(), result.end(), uint32_t(value)) == result.end(),
                    "Duplicate M2 sequence slot");
        result.push_back(uint32_t(value));
        if (comma == remaining.npos)
            break;
        remaining.remove_prefix(comma + 1);
    }
    return result;
}
M2PreviewPlaybackRequest
prepareM2PreviewPlayback(std::shared_ptr<const M2PreviewSequenceCatalog> source,
                         M2PreviewPlaybackRequest request) {
    m2::require(source && !source->empty() && source->size() <= 32, "Missing M2 sequence catalog");
    auto validSpeed = [](double speed) {
        m2::require(std::isfinite(speed) && speed >= 0 && speed <= 1024,
                    "Invalid M2 playback speed");
    };
    switch (request.action) {
    case M2PreviewPlaybackAction::Play:
        m2::require(std::any_of(source->begin(), source->end(),
                                [&](const auto &s) { return s.index == request.sequence; }),
                    "M2 sequence was not preloaded");
        validSpeed(request.options.speed);
        break;
    case M2PreviewPlaybackAction::Speed:
        validSpeed(request.speed);
        break;
    case M2PreviewPlaybackAction::Pause:
    case M2PreviewPlaybackAction::Seek:
    case M2PreviewPlaybackAction::Loop:
        break;
    default:
        m2::require(false, "Invalid M2 playback action");
    }
    request.source = std::move(source);
    return request;
}
void M2PreviewPlaybackQueue::push(M2PreviewPlaybackRequest request) {
    m2::require(requests_.size() < 64, "M2 playback command queue is full");
    request = prepareM2PreviewPlayback(request.source, request);
    requests_.push_back(std::move(request));
}
std::deque<M2PreviewPlaybackRequest> M2PreviewPlaybackQueue::take() {
    std::deque<M2PreviewPlaybackRequest> result;
    result.swap(requests_);
    return result;
}
} // namespace wow::android
