#pragma once
#include <cstddef>
#include <mutex>
namespace wow::android {
// Serializes asset-worker archive reads (MPQ mounts are process-global).
std::mutex &assetIoLock();
// Unit model requests not yet loaded (network snapshot -> Java scheduling).
size_t worldAssetWork();
} // namespace wow::android
