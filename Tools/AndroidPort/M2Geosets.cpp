#include "M2Geosets.h"
#include "M2File.h"
#include <algorithm>
#include <limits>
#include <string_view>
namespace wow::android {
namespace {
std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == s.npos)
        return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
void validateIds(std::vector<uint32_t> &ids) {
    m2::require(ids.size() <= 1024, "Too many M2 geoset IDs");
    std::sort(ids.begin(), ids.end());
    m2::require(std::adjacent_find(ids.begin(), ids.end()) == ids.end(), "Duplicate M2 geoset ID");
}
} // namespace
std::shared_ptr<const M2GeosetCatalog> makeM2GeosetCatalog(const M2Geometry &geometry) {
    m2::require(geometry.sections.size() <= 65536, "Too many M2 geoset sections");
    auto result = std::make_shared<M2GeosetCatalog>();
    for (const auto &s : geometry.sections)
        result->sections.push_back(s.m_skinSectionId);
    return result;
}
M2GeosetIds parseM2Geosets(const std::string &text) {
    m2::require(text.size() <= 65536, "M2 geoset text too long");
    auto remaining = trim(text);
    if (remaining == "*")
        return std::nullopt;
    std::vector<uint32_t> ids;
    if (remaining.empty())
        return ids;
    while (true) {
        const auto comma = remaining.find(',');
        const auto token = trim(remaining.substr(0, comma));
        m2::require(!token.empty() && token.size() <= 10, "Invalid M2 geoset ID");
        uint64_t value = 0;
        for (char c : token) {
            m2::require(c >= '0' && c <= '9', "Invalid M2 geoset decimal ID");
            value = value * 10 + unsigned(c - '0');
        }
        m2::require(value <= std::numeric_limits<uint32_t>::max(), "M2 geoset ID overflow");
        ids.push_back(uint32_t(value));
        m2::require(ids.size() <= 1024, "Too many M2 geoset IDs");
        if (comma == remaining.npos)
            break;
        remaining.remove_prefix(comma + 1);
    }
    validateIds(ids);
    return ids;
}
std::shared_ptr<const M2GeosetSelection>
prepareM2Geosets(std::shared_ptr<const M2GeosetCatalog> source, const M2GeosetIds &ids) {
    m2::require(source && source->sections.size() <= 65536, "Invalid M2 geoset catalog");
    auto selected = ids.value_or(std::vector<uint32_t>{});
    validateIds(selected);
    // Sort a copy once: bound work independently of repeated section IDs.
    auto available = source->sections;
    std::sort(available.begin(), available.end());
    for (uint32_t id : selected)
        m2::require(std::binary_search(available.begin(), available.end(), id),
                    "Unknown M2 geoset ID");
    auto result = std::make_shared<M2GeosetSelection>();
    result->source = std::move(source);
    for (uint32_t id : result->source->sections)
        result->visible.push_back(!ids || std::binary_search(selected.begin(), selected.end(), id));
    return result;
}
} // namespace wow::android
