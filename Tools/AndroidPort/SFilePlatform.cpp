#include "ArchiveReader.h"
#include "NativeAssetPaths.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Storm/H/storm.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

// Android replacement for synchronous SFile2 entry points. Do not link the
// Windows SFile2-Core.cpp or original Mopaq internals into this target as well.
namespace {
namespace fs = std::filesystem;
using wow::android::ArchiveReader;
using wow::android::ArchiveStream;
struct Mount {
    std::shared_ptr<ArchiveReader> reader;
    std::string prefix;
    int32_t priority;
    uint64_t sequence;
};
struct Mounted { SArchive* owner; Mount mount; };
struct Registry {
    std::mutex mutex;
    std::vector<Mounted> mounts;
    uint64_t sequence = 0;
    uint32_t directAccess = 0;
    bool checkDisk = true;
    char dataPath[260]{};
    char alternatePath[260]{};
};
Registry& registry() { static Registry value; return value; }
thread_local std::string errorDetails;
int fail(uint32_t code, const char* message) {
    SErrSetLastError(code); errorDetails = message; return 0;
}
int exceptionError() {
    try { throw; }
    catch (const wow::android::ArchiveError& e) { return fail(e.code, e.what()); }
    catch (const std::invalid_argument& e) { return fail(87, e.what()); }
    catch (const std::length_error& e) { return fail(8, e.what()); }
    catch (const std::exception& e) { return fail(0x85100083, e.what()); }
}
std::string assetName(const char* input) {
    if (!input || !*input) throw std::invalid_argument("Empty resource name");
    std::string name(input);
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.front() == '/' || name.find(':') != std::string::npos)
        throw std::invalid_argument("Absolute resource name");
    for (const auto& part : fs::path(name))
        if (part == "." || part == "..") throw std::invalid_argument("Resource path traversal");
    if (name.find("//") != std::string::npos || name.back() == '/')
        throw std::invalid_argument("Empty resource path component");
    return name;
}
bool equalCase(const std::string& left, const std::string& right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
}
fs::path nativePath(const char* input) {
    if (!input) throw std::invalid_argument("Null filesystem path");
    std::string path(input);
    std::replace(path.begin(), path.end(), '\\', '/');
    return fs::path(path);
}
using wow::android::resolvePhysicalPath;
bool copyPath(char* destination, uint32_t size, const std::string& value) {
    if (!destination || !size || value.size() >= size) { fail(87, "Path buffer too small"); return false; }
    std::memcpy(destination, value.c_str(), value.size() + 1); return true;
}
bool setPath(char* destination, const char* path) {
    auto value = nativePath(path).string();
    if (!value.empty() && value.back() != '/') value += '/';
    return copyPath(destination, 260, value);
}
struct File {
    std::unique_ptr<ArchiveStream> archive;
    std::ifstream disk;
    uint64_t length = 0, position = 0;
    explicit File(std::unique_ptr<ArchiveStream> entry) : archive(std::move(entry)), length(archive->size()) {}
    explicit File(const fs::path& path) : disk(path, std::ios::binary) {
        if (!disk) throw std::runtime_error("Cannot open loose asset");
        length = fs::file_size(path);
    }
    uint32_t read(void* buffer, uint32_t count) {
        uint32_t amount = archive ? archive->read(buffer, count) : 0;
        if (!archive && count) {
            disk.read(static_cast<char*>(buffer), count);
            amount = static_cast<uint32_t>(disk.gcount());
            if (disk.bad()) throw std::runtime_error("Loose asset read failed");
        }
        position += amount;
        return amount;
    }
    uint64_t seek(int64_t distance, unsigned origin) {
        if (origin > 2) throw std::invalid_argument("Invalid seek origin");
        if (archive) return position = archive->seek(distance, origin);
        const uint64_t base = origin == 0 ? 0 : origin == 1 ? position : length;
        uint64_t target;
        if (distance < 0) {
            const auto magnitude = uint64_t(-(distance + 1)) + 1;
            if (magnitude > base) throw std::invalid_argument("Negative seek");
            target = base - magnitude;
        } else target = base + std::min(uint64_t(distance), length - std::min(base, length));
        disk.clear(); disk.seekg(static_cast<std::streamoff>(target));
        if (!disk) throw std::runtime_error("Loose asset seek failed");
        return position = target;
    }
};
std::unique_ptr<File> loose(const std::string& name, const std::vector<fs::path>& roots) {
    for (const auto& root : roots) {
        const auto path = resolvePhysicalPath(root / name);
        if (path.empty() || !fs::is_regular_file(path)) continue;
        const auto resolvedRoot = fs::weakly_canonical(fs::absolute(root));
        const auto resolved = fs::weakly_canonical(path);
        const auto relative = resolved.lexically_relative(resolvedRoot);
        if (relative.empty() || *relative.begin() == "..")
            throw std::invalid_argument("Loose resource escaped its data root");
        return std::make_unique<File>(path);
    }
    return {};
}
std::unique_ptr<File> open(SArchive* selected, const char* input, uint32_t flags, bool archivesOnly = false) {
    auto name = assetName(input);
    std::vector<Mount> mounts;
    std::vector<fs::path> roots;
    uint32_t direct;
    bool checkDisk;
    {
        auto& state = registry(); std::lock_guard<std::mutex> lock(state.mutex);
        direct = state.directAccess; checkDisk = state.checkDisk;
        roots = {fs::path(SFile::m_basePath), fs::path(state.dataPath), fs::path(state.alternatePath)};
        if (selected) {
            if (!selected->m_portBackend) throw std::invalid_argument("Invalid archive");
            mounts.push_back(*static_cast<Mount*>(selected->m_portBackend));
        } else for (const auto& item : state.mounts) mounts.push_back(item.mount);
    }
    // Flag 2 (also search loose files by base name, e.g. fonts in the OS font folder) has
    // no Android meaning: archives and the data roots are searched as usual.
    if (!archivesOnly && !selected && checkDisk && ((flags | direct) & 1)) {
        roots.erase(std::remove_if(roots.begin(), roots.end(), [](const auto& p) { return p.empty(); }), roots.end());
        if (auto file = loose(name, roots)) return file;
    }
    std::stable_sort(mounts.begin(), mounts.end(), [](const auto& a, const auto& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.sequence > b.sequence;
    });
    for (const auto& mount : mounts) {
        const auto qualified = mount.prefix + name;
        const auto status = mount.reader->status(qualified);
        if (status == wow::android::ArchiveEntryStatus::Deleted)
            throw wow::android::ArchiveError("Resource removed by patch archive", 2);
        if (status == wow::android::ArchiveEntryStatus::Available)
            return std::make_unique<File>(mount.reader->open(qualified));
    }
    throw wow::android::ArchiveError("Resource not found", 2);
}
char* duplicate(const char* text) {
    const auto size = std::strlen(text) + 1;
    auto* result = static_cast<char*>(SMemAlloc(size, __FILE__, __LINE__, 0));
    std::memcpy(result, text, size); return result;
}
}

uint32_t SFile::s_locale = 0;
char SFile::m_basePath[260]{};
SFileLock::SFileLock() { Blizzard::Lock::MutexCreate(m_mutex); }
SFileLock::~SFileLock() { Blizzard::Lock::MutexDestroy(m_mutex); }
SFileImpl::SFileImpl(SFILE_TYPE type) : m_type(type), m_stream(nullptr), m_archive(nullptr),
    m_name(nullptr), m_actualName(nullptr), m_size(0), m_data(nullptr), m_position(0),
    m_file(nullptr), unused_0024(0), unused_0028(0), m_lock(new SFileLock) {}
SFileImpl::~SFileImpl() {
    delete static_cast<File*>(m_portBackend);
    SMemFree(m_name, __FILE__, __LINE__, 0); SMemFree(m_actualName, __FILE__, __LINE__, 0);
    SMemFree(m_data, __FILE__, __LINE__, 0); delete m_lock;
}
int32_t SFile::Initialize() { registry(); return 1; }
void SFile::Destroy() {
    auto& state = registry(); std::lock_guard<std::mutex> lock(state.mutex);
    for (auto& item : state.mounts) { delete static_cast<Mount*>(item.owner->m_portBackend); delete item.owner; }
    state.mounts.clear();
}
int32_t SFile::OpenArchive(const char* input, int32_t priority, uint32_t, SArchive** output) {
    if (!output) return fail(87, "Null archive output");
    *output = nullptr;
    try {
        auto path = nativePath(input);
        if (path.empty()) return fail(87, "Empty archive path");
        if (path.is_relative()) {
            auto& state = registry(); std::lock_guard<std::mutex> lock(state.mutex);
            path = fs::path(m_basePath) / path;
        }
        path = resolvePhysicalPath(path);
        if (path.empty()) return fail(2, "Archive path not found");
        auto reader = std::make_shared<ArchiveReader>(path);
        auto handle = std::make_unique<SArchive>(); handle->m_type = SARCHIVE_MPQ; handle->m_archive = nullptr;
        auto& state = registry(); std::lock_guard<std::mutex> lock(state.mutex);
        auto mount = std::make_unique<Mount>(Mount{std::move(reader), "", priority, ++state.sequence});
        state.mounts.push_back({handle.get(), *mount});
        handle->m_portBackend = mount.release(); *output = handle.release(); return 1;
    } catch (...) { return exceptionError(); }
}
int32_t SFile::OpenPathAsArchive(SArchive* parent, const char* input, int32_t priority, uint32_t, SArchive** output) {
    if (!output) return fail(87, "Null archive-view output");
    *output = nullptr;
    try {
        const auto prefix = assetName(input) + '/';
        auto& state = registry(); std::lock_guard<std::mutex> lock(state.mutex);
        if (!parent || !parent->m_portBackend) return fail(87, "Invalid parent archive");
        auto mount = std::make_unique<Mount>(*static_cast<Mount*>(parent->m_portBackend));
        mount->prefix += prefix; mount->priority = priority; mount->sequence = ++state.sequence;
        auto handle = std::make_unique<SArchive>(); handle->m_type = SARCHIVE_MPQ; handle->m_archive = nullptr;
        state.mounts.push_back({handle.get(), *mount}); handle->m_portBackend = mount.release();
        *output = handle.release(); return 1;
    } catch (...) { return exceptionError(); }
}
int32_t SFile::CloseArchive(SArchive* handle) {
    if (!handle) return fail(87, "Null archive handle");
    auto& state = registry(); std::lock_guard<std::mutex> lock(state.mutex);
    auto it = std::find_if(state.mounts.begin(), state.mounts.end(), [&](const auto& m) { return m.owner == handle; });
    if (it == state.mounts.end()) return fail(87, "Unknown archive handle");
    state.mounts.erase(it); delete static_cast<Mount*>(handle->m_portBackend); delete handle; return 1;
}
// Called with each file name opened through SFile (nullptr: off).
void (*g_sfileOpenObserver)(const char* name) = nullptr;
int32_t SFile::OpenEx(SArchive* archive, const char* name, uint32_t flags, SFileImpl** output) {
    if (!output) return fail(87, "Null file output");
    *output = nullptr;
    try {
        auto stream = open(archive, name, flags);
        auto file = std::make_unique<SFileImpl>(stream->archive ? SFILE_OLD_SFILE : SFILE_PLAIN);
        file->m_name = duplicate(name);
        file->m_size = static_cast<uint32_t>(std::min(stream->length, uint64_t(UINT32_MAX)));
        if (g_sfileOpenObserver) g_sfileOpenObserver(name); // Android port: debug record of files read
        file->m_portBackend = stream.release(); *output = file.release(); return 1;
    } catch (...) { return exceptionError(); }
}
int32_t SFileReadFile(SFileImpl* file, void* buffer, uint32_t count, uint32_t* received, SOVERLAPPED* overlapped, uint32_t) {
    if (received) *received = 0;
    if (!file || !file->m_portBackend || (!buffer && count)) return fail(87, "Invalid file read");
    if (overlapped) return fail(50, "Overlapped reads are not implemented");
    Blizzard::Lock::ScopedMutex lock(file->m_lock->m_mutex);
    try {
        auto& stream = *static_cast<File*>(file->m_portBackend);
        const auto amount = stream.read(buffer, count); file->m_position = static_cast<uint32_t>(stream.position);
        if (received) *received = amount;
        return amount == count ? 1 : fail(38, "End of resource");
    } catch (...) { return exceptionError(); }
}
uint32_t SFileGetFileSize(SFileImpl* file, uint32_t* high) {
    if (high) *high = 0;
    if (!file || !file->m_portBackend) { fail(87, "Invalid file size query"); return UINT32_MAX; }
    const auto size = static_cast<File*>(file->m_portBackend)->length;
    if (high) *high = static_cast<uint32_t>(size >> 32);
    return static_cast<uint32_t>(size);
}
int32_t SFile::SetFilePointer(SFileImpl* file, int32_t distance, int32_t* high, uint32_t origin) {
    if (!file || !file->m_portBackend) { fail(87, "Invalid file seek"); return -1; }
    Blizzard::Lock::ScopedMutex lock(file->m_lock->m_mutex);
    try {
        const int64_t delta = high ? static_cast<int64_t>((uint64_t(static_cast<uint32_t>(*high)) << 32) | static_cast<uint32_t>(distance)) : distance;
        const auto position = static_cast<File*>(file->m_portBackend)->seek(delta, origin);
        file->m_position = static_cast<uint32_t>(position); if (high) *high = static_cast<int32_t>(position >> 32);
        return static_cast<int32_t>(position);
    } catch (...) { exceptionError(); return -1; }
}
int32_t SFileCloseFile(SFileImpl* file) { delete file; return 1; }
int32_t SFileOpenFile(const char* name, SFileImpl** output) { return SFile::OpenEx(nullptr, name, 0, output); }
int32_t SFile::FileExists(const char* name) { return FileExistsEx(name, 0); }
int32_t SFile::FileExistsEx(const char* name, uint32_t flags) {
    try { return open(nullptr, name, flags) != nullptr; } catch (...) { return exceptionError(); }
}
int32_t SFile::FileExistsInArchives(const char* name) {
    try { return open(nullptr, name, 0, true) != nullptr; } catch (...) { return exceptionError(); }
}
int32_t SFile::ReadFile(SArchive* archive, const char* name, void** data, uint32_t* size, uint32_t extra, uint32_t flags, SOVERLAPPED* overlapped) {
    if (data) *data = nullptr;
    if (size) *size = 0;
    if (!data) return fail(87, "Null resource output");
    if (overlapped) return fail(50, "Overlapped reads are not implemented");
    SFileImpl* raw = nullptr;
    if (!OpenEx(archive, name, flags, &raw)) return 0;
    std::unique_ptr<SFileImpl> file(raw);
    const uint64_t length = static_cast<File*>(file->m_portBackend)->length;
    if (length + extra > 256 * 1024 * 1024) return fail(8, "Whole-resource read exceeds 256 MiB; use streaming reads");
    void* bytes = SMemAlloc(static_cast<size_t>(length) + extra, __FILE__, __LINE__, SMEM_FLAG_ZEROMEMORY);
    if (!bytes) return fail(8, "Resource allocation failed");
    if (!SFileReadFile(file.get(), bytes, static_cast<uint32_t>(length), nullptr, nullptr, 0)) {
        SMemFree(bytes, __FILE__, __LINE__, 0); return 0;
    }
    *data = bytes; if (size) *size = static_cast<uint32_t>(length); return 1;
}
int32_t SFile::LoadFile(const char* name, void** data, uint32_t* size, uint32_t extra, SOVERLAPPED* overlapped) {
    return ReadFile(nullptr, name, data, size, extra, 0, overlapped);
}
int32_t SFileLoadFileEx(SArchive* archive, const char* name, void** data, uint32_t* size, uint32_t extra, uint32_t flags, SOVERLAPPED* overlapped) {
    return SFile::ReadFile(archive, name, data, size, extra, flags, overlapped);
}
int32_t SFile::Unload(void* data) { return SMemFree(data, __FILE__, __LINE__, 0); }
int32_t SFileUnloadFile(void* data) { return SFile::Unload(data); }
int32_t SFile::GetActualFileName(SFileImpl* file, char* buffer, uint32_t size) {
    return file && file->m_name ? copyPath(buffer, size, file->m_name) : fail(87, "Invalid file name query");
}
int32_t SFile::SetBasePath(const char* path) {
    try { auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); return setPath(m_basePath, path); }
    catch (...) { return exceptionError(); }
}
int32_t SFile::GetBasePath(char* buffer, uint32_t size) {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); return copyPath(buffer, size, m_basePath);
}
int32_t SFile::SetDataPath(const char* path) {
    try { auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); return setPath(r.dataPath, path); }
    catch (...) { return exceptionError(); }
}
int32_t SFile::SetDataPathAlternate(const char* path) {
    try { auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); return setPath(r.alternatePath, path); }
    catch (...) { return exceptionError(); }
}
const char* SFile::GetDataPath() { return registry().dataPath; }
const char* SFile::GetErrorDetails() { return errorDetails.c_str(); }
int32_t SFile::EnableDirectAccess(uint32_t flags) {
    if (flags & ~1u) return fail(50, "Only relative direct-access mode is supported");
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); r.directAccess = flags; return 1;
}
void SFile::DisableSFileCheckDisk() {
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex); r.checkDisk = false;
}
int32_t SFileOpenArchive(const char* path, int32_t priority, uint32_t flags, SArchive** output) {
    return SFile::OpenArchive(path, priority, flags, output);
}
int32_t SFileCloseArchive(SArchive* archive) { return SFile::CloseArchive(archive); }
int32_t SFile::List(SArchive* archive, int32_t (*callback)(const char*, void*), void* context) {
    if (!archive || !callback) return fail(87, "Invalid archive listing");
    void* data = nullptr; uint32_t length = 0;
    if (!ReadFile(archive, "(listfile)", &data, &length, 1, 0, nullptr)) return 0;
    struct Release { void* data; ~Release() { SFile::Unload(data); } } release{data};
    auto* text = static_cast<char*>(data);
    uint32_t begin = 0;
    for (uint32_t i = 0; i <= length; ++i) {
        if (i < length && text[i] != '\r' && text[i] != '\n') continue;
        text[i] = 0;
        if (i > begin) {
            const std::string name(text + begin);
            if (!equalCase(fs::path(name).extension().string(), ".md5") && !callback(name.c_str(), context)) break;
        }
        begin = i + 1;
    }
    return 1;
}
