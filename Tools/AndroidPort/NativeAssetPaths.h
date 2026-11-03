#pragma once
#include <filesystem>
namespace wow::android {
std::filesystem::path resolvePhysicalPath(const std::filesystem::path& input);
}
