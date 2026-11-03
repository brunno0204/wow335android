#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace wow::android {
struct ArchiveState;
enum class ArchiveEntryStatus { Missing, Available, Deleted };
class ArchiveError : public std::runtime_error {
public:
    uint32_t code;
    ArchiveError(const char* action, uint32_t error);
};
class ArchiveStream {
public:
    ~ArchiveStream();
    ArchiveStream(const ArchiveStream&) = delete;
    ArchiveStream& operator=(const ArchiveStream&) = delete;
    uint64_t size() const;
    uint64_t seek(int64_t distance, unsigned origin);
    uint32_t read(void* buffer, uint32_t count);
private:
    friend class ArchiveReader;
    ArchiveStream(std::shared_ptr<ArchiveState> archive, void* file);
    std::shared_ptr<ArchiveState> archive_;
    void* file_;
};
// Read-only archive ownership. Patches are attached in explicit oldest-to-newest
// order; callers must not infer a full game mount order from a directory sort.
class ArchiveReader {
public:
    explicit ArchiveReader(const std::filesystem::path& archive);
    ~ArchiveReader();
    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;
    void attachPatch(const std::filesystem::path& archive, const char* prefix = nullptr);
    bool contains(const std::string& logicalPath) const;
    ArchiveEntryStatus status(const std::string& logicalPath) const;
    std::unique_ptr<ArchiveStream> open(const std::string& logicalPath) const;
    std::vector<std::uint8_t> read(const std::string& logicalPath,
        std::size_t maxBytes = 64 * 1024 * 1024) const;
private:
    std::shared_ptr<ArchiveState> state_;
    static std::string normalize(const std::string& path);
};
}
