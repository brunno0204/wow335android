#pragma once
#include "M2Geometry.h"
#include <memory>
#include <optional>
namespace wow::android {
// Exact full uint32 SKIN section IDs, not batch indices or automatic equipment
// groups.
using M2GeosetIds = std::optional<std::vector<uint32_t>>; // nullopt = all; empty = none
struct M2GeosetCatalog {
    std::vector<uint32_t> sections;
};
struct M2GeosetSelection {
    std::shared_ptr<const M2GeosetCatalog> source;
    std::vector<uint8_t> visible;
};
std::shared_ptr<const M2GeosetCatalog> makeM2GeosetCatalog(const M2Geometry &geometry);
// Bounded development syntax: '*' = all; empty = none; otherwise
// comma-separated decimal IDs.
M2GeosetIds parseM2Geosets(const std::string &text);
std::shared_ptr<const M2GeosetSelection>
prepareM2Geosets(std::shared_ptr<const M2GeosetCatalog> source, const M2GeosetIds &ids);
} // namespace wow::android
