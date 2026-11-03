#include "ArchiveReader.h"
#include <StormLib.h>
#include <limits>
#include <stdexcept>

namespace wow::android {
struct ArchiveState {
    HANDLE handle = nullptr;
    mutable std::mutex mutex;
    bool patched = false;
    ~ArchiveState() { if (handle) SFileCloseArchive(handle); }
};
ArchiveError::ArchiveError(const char* action, uint32_t error)
    : std::runtime_error(std::string(action) + " (MPQ error " + std::to_string(error) + ")"), code(error) {}
namespace {
ArchiveError archiveError(const char* action) { return ArchiveError(action, SErrGetLastError()); }
}
ArchiveReader::ArchiveReader(const std::filesystem::path& archive) : state_(std::make_shared<ArchiveState>()) {
    if (!SFileOpenArchive(archive.c_str(), 0, MPQ_OPEN_READ_ONLY, &state_->handle))
        throw archiveError("Cannot open archive");
}
ArchiveReader::~ArchiveReader() = default;
std::string ArchiveReader::normalize(const std::string& input) {
    if (input.empty() || input.find('\0') != std::string::npos || input.front() == '/' ||
        input.front() == '\\' || input.find(':') != std::string::npos)
        throw std::invalid_argument("Expected a relative archive path");
    std::string result = input;
    for (char& c : result) if (c == '/') c = '\\';
    std::size_t start = 0;
    while (start <= result.size()) {
        auto end = result.find('\\', start);
        auto component = result.substr(start, end - start);
        if (component.empty() || component == "." || component == "..")
            throw std::invalid_argument("Invalid archive path component");
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}
void ArchiveReader::attachPatch(const std::filesystem::path& archive, const char* prefix) {
    std::lock_guard<std::mutex> guard(state_->mutex);
    if (!SFileOpenPatchArchive(state_->handle, archive.c_str(), prefix, 0))
        throw archiveError("Cannot attach patch archive");
    state_->patched = true;
}
bool ArchiveReader::contains(const std::string& logicalPath) const {
    return status(logicalPath) == ArchiveEntryStatus::Available;
}
ArchiveEntryStatus ArchiveReader::status(const std::string& logicalPath) const {
    const auto path = normalize(logicalPath);
    std::lock_guard<std::mutex> guard(state_->mutex);
    HANDLE file = nullptr;
    if (!SFileOpenFileEx(state_->handle, path.c_str(), SFILE_OPEN_FROM_MPQ, &file)) {
        const auto error = SErrGetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return ArchiveEntryStatus::Missing;
        if (error == ERROR_FILE_DELETED) return ArchiveEntryStatus::Deleted;
        throw ArchiveError("Cannot inspect archive entry", error);
    }
    DWORD flags = 0;
    bool read = SFileGetFileInfo(file, SFileInfoFlags, &flags, sizeof(flags), nullptr);
    const auto error = SErrGetLastError();
    SFileCloseFile(file);
    if (!read) throw ArchiveError("Cannot inspect entry flags", error);
    return flags & MPQ_FILE_DELETE_MARKER ? ArchiveEntryStatus::Deleted : ArchiveEntryStatus::Available;
}
std::unique_ptr<ArchiveStream> ArchiveReader::open(const std::string& logicalPath) const {
    const auto path = normalize(logicalPath);
    std::lock_guard<std::mutex> guard(state_->mutex);
    HANDLE file = nullptr;
    if (!SFileOpenFileEx(state_->handle, path.c_str(), SFILE_OPEN_FROM_MPQ, &file))
        throw archiveError("Cannot open archive entry");
    DWORD flags = 0;
    if (!SFileGetFileInfo(file, SFileInfoFlags, &flags, sizeof(flags), nullptr)) {
        const auto error = SErrGetLastError(); SFileCloseFile(file);
        throw ArchiveError("Cannot inspect entry flags", error);
    }
    if ((flags & MPQ_FILE_DELETE_MARKER) || ((flags & MPQ_FILE_PATCH_FILE) && !state_->patched)) {
        SFileCloseFile(file);
        throw ArchiveError("Deleted entry or delta patch without its base chain",
            flags & MPQ_FILE_DELETE_MARKER ? ERROR_FILE_DELETED : ERROR_NOT_SUPPORTED);
    }
    try { return std::unique_ptr<ArchiveStream>(new ArchiveStream(state_, file)); }
    catch (...) { SFileCloseFile(file); throw; }
}
std::vector<std::uint8_t> ArchiveReader::read(const std::string& logicalPath, std::size_t limit) const {
    auto file = open(logicalPath);
    const auto size = file->size();
    if (size > limit || size > std::numeric_limits<uint32_t>::max())
        throw std::length_error("Archive entry exceeds the configured read limit");
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
    if (size && file->read(result.data(), static_cast<uint32_t>(size)) != size)
        throw ArchiveError("Truncated archive entry", ERROR_HANDLE_EOF);
    return result;
}
ArchiveStream::ArchiveStream(std::shared_ptr<ArchiveState> archive, void* file)
    : archive_(std::move(archive)), file_(file) {}
ArchiveStream::~ArchiveStream() {
    std::lock_guard<std::mutex> guard(archive_->mutex);
    SFileCloseFile(file_);
}
uint64_t ArchiveStream::size() const {
    std::lock_guard<std::mutex> guard(archive_->mutex);
    DWORD high = 0;
    SErrSetLastError(ERROR_SUCCESS);
    const DWORD low = SFileGetFileSize(file_, &high);
    if (low == SFILE_INVALID_SIZE && SErrGetLastError() != ERROR_SUCCESS)
        throw archiveError("Cannot determine entry size");
    return (uint64_t(high) << 32) | low;
}
uint64_t ArchiveStream::seek(int64_t distance, unsigned origin) {
    if (origin > 2) throw std::invalid_argument("Invalid seek origin");
    std::lock_guard<std::mutex> guard(archive_->mutex);
    LONG high = static_cast<LONG>(static_cast<uint64_t>(distance) >> 32);
    SErrSetLastError(ERROR_SUCCESS);
    DWORD low = SFileSetFilePointer(file_, static_cast<LONG>(distance), &high, origin);
    if (low == SFILE_INVALID_POS && SErrGetLastError() != ERROR_SUCCESS)
        throw archiveError("Cannot seek archive entry");
    return (uint64_t(static_cast<uint32_t>(high)) << 32) | low;
}
uint32_t ArchiveStream::read(void* buffer, uint32_t count) {
    if (!count) return 0;
    if (!buffer) throw std::invalid_argument("Null read buffer");
    std::lock_guard<std::mutex> guard(archive_->mutex);
    DWORD received = 0;
    if (!SFileReadFile(file_, buffer, count, &received, nullptr) && SErrGetLastError() != ERROR_HANDLE_EOF)
        throw archiveError("Cannot read archive entry");
    return received;
}
}
