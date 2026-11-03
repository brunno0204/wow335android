#include "GlueResources.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include <stdexcept>
namespace wow::android {
std::string normalizeGlueResourcePath(const std::string &input) {
    if (input.empty() || input.size() > 512)
        throw std::invalid_argument("Invalid glue resource path length");
    std::string path;
    path.reserve(input.size());
    for (unsigned char c : input) {
        if (c < 32 || c > 126 || c == ':' || c == '\0')
            throw std::invalid_argument("Invalid glue resource path byte");
        if (c == '\\')
            c = '/';
        if (c >= 'A' && c <= 'Z')
            c = uint8_t(c - 'A' + 'a');
        path += char(c);
    }
    size_t first = 0;
    while (first <= path.size()) {
        const auto next = path.find('/', first);
        const auto part = path.substr(first, next == std::string::npos ? next : next - first);
        if (part.empty() || part == "." || part == "..")
            throw std::invalid_argument("Invalid glue path component");
        if (next == std::string::npos)
            break;
        first = next + 1;
    }
    const auto ends = [&](const char *suffix) {
        const std::string s = suffix;
        return path.size() >= s.size() && path.compare(path.size() - s.size(), s.size(), s) == 0;
    };
    const bool layout = path.rfind("interface/gluexml/", 0) == 0 && (ends(".xml") || ends(".toc"));
    const bool texture = path.rfind("interface/", 0) == 0 && (ends(".blp") || ends(".tga"));
    const bool font = path.rfind("fonts/", 0) == 0 && ends(".ttf");
    if (!layout && !texture && !font)
        throw std::invalid_argument("Not a permitted glue layout/art/font resource");
    return path;
}
std::optional<std::vector<uint8_t>> readMountedGlueResource(const std::string &input) {
    const auto path = normalizeGlueResourcePath(input);
    SFileImpl *file = nullptr;
    if (!SFileOpenFile(path.c_str(), &file))
        return std::nullopt;
    struct Close {
        SFileImpl *file;
        ~Close() { SFileCloseFile(file); }
    } close{file};
    uint32_t high = 0, size = SFileGetFileSize(file, &high);
    if (high || size > 8 * 1024 * 1024)
        throw std::length_error("Glue resource exceeds8MiB");
    std::vector<uint8_t> bytes(size);
    uint32_t read = 0;
    if (size && (!SFileReadFile(file, bytes.data(), size, &read, nullptr, 0) || read != size))
        throw std::runtime_error("Incomplete glue resource read");
    return bytes;
}
} // namespace wow::android
