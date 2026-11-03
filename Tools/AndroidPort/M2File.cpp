#include "M2File.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
namespace wow::android::m2 {
std::vector<uint8_t> readFile(const std::string &path, uint32_t limit) {
    SFileImpl *file = nullptr;
    require(!path.empty() && path.find('\0') == std::string::npos, "Invalid M2 asset path");
    if (!SFileOpenFile(path.c_str(), &file))
        throw std::runtime_error("Cannot open M2 asset");
    struct Close {
        SFileImpl *file;
        ~Close() { SFileCloseFile(file); }
    } close{file};
    uint32_t high = 0, size = SFileGetFileSize(file, &high);
    require(!high && size <= limit, "M2 asset exceeds file budget");
    std::vector<uint8_t> bytes(size);
    uint32_t received = 0;
    if (size && (!SFileReadFile(file, bytes.data(), size, &received, nullptr, 0) || received != size))
        throw std::runtime_error("Incomplete M2 asset read");
    return bytes;
}
} // namespace wow::android::m2
