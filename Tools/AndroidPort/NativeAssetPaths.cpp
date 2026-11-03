#include "NativeAssetPaths.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
namespace wow::android {
namespace fs = std::filesystem;
namespace {
bool same(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}
bool missing(std::error_code code) {
    return code == std::errc::no_such_file_or_directory || code == std::errc::not_a_directory;
}
}
fs::path resolvePhysicalPath(const fs::path& input) {
    std::error_code ec;
    auto full = fs::absolute(input, ec).lexically_normal();
    if (ec) throw fs::filesystem_error("Cannot resolve asset path", input, ec);
    fs::path current = full.root_path();
    for (const auto& component : full.relative_path()) {
        auto exact = current / component;
        if (fs::exists(exact, ec)) { current = exact; continue; }
        if (ec && !missing(ec)) throw fs::filesystem_error("Cannot inspect asset path", exact, ec);
        ec.clear();
        fs::path found;
        for (fs::directory_iterator it(current, ec), end; !ec && it != end; it.increment(ec)) {
            if (same(it->path().filename().string(), component.string())) {
                if (!found.empty()) throw std::invalid_argument("Ambiguous case-insensitive asset path");
                found = it->path();
            }
        }
        if (ec && !missing(ec)) throw fs::filesystem_error("Cannot scan asset directory", current, ec);
        if (found.empty()) return {};
        current = found;
    }
    return current;
}
}
