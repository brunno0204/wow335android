#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
namespace wow::android {
// Only layout/art/font resources; never Lua, WTF/config, passwords or account data.
std::string normalizeGlueResourcePath(const std::string &);
// Installation mounts must be owned by the single asset worker. Missing files
// return nullopt; reads cap at8MiB and preserve caller-owned bytes after unmount.
std::optional<std::vector<uint8_t>> readMountedGlueResource(const std::string &);
} // namespace wow::android
