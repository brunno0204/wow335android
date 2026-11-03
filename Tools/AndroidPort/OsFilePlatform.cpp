#include "Engine/Source/Os/OsFile.h"
#include "Engine/Source/Storm/H/storm.h"
#include "NativeAssetPaths.h"
#include <filesystem>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>
#include <memory>

struct HOSFILE__ { int fd; std::mutex mutex; explicit HOSFILE__(int value) : fd(value) {} };
namespace {
namespace fs = std::filesystem;
int failure(int code = errno) {
    uint32_t mapped = code == ENOENT ? 2 : code == EACCES || code == EPERM ? 5 : code == EBADF ? 6 :
        code == EINVAL ? 87 : code == EEXIST ? 80 : code == ENOSPC ? 112 : code == ENOMEM ? 8 : code == ENOTSUP ? 50 : 31;
    SErrSetLastError(mapped); return 0;
}
fs::path path(const char* value) {
    if (!value || !*value) throw std::invalid_argument("Empty native file path");
    std::string result(value); std::replace(result.begin(), result.end(), '\\', '/'); return fs::path(result);
}
bool valid(HOSFILE file) { return file && file != OS_INVALID_FILE; }
bool writable(fs::perms permissions) {
    return (permissions & (fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write)) != fs::perms::none;
}
uint32_t attributes(const fs::path& file) {
    const auto status = fs::status(file);
    if (!fs::exists(status)) { failure(ENOENT); return UINT32_MAX; }
    uint32_t flags = fs::is_directory(status) ? OS_FILE_ATTRIBUTE_DIRECTORY : OS_FILE_ATTRIBUTE_NORMAL;
    if (!writable(status.permissions())) flags |= 1;
    const auto name = file.filename().string();
    if (!name.empty() && name.front() == '.') flags |= 2;
    return flags;
}
bool matches(const char* name, const char* pattern) {
    const char *star = nullptr, *retry = nullptr;
    while (*name) {
        if (*pattern == '?' || std::tolower(static_cast<unsigned char>(*name)) == std::tolower(static_cast<unsigned char>(*pattern))) { ++name; ++pattern; }
        else if (*pattern == '*') { star = pattern++; retry = name; }
        else if (star) { pattern = star + 1; name = ++retry; }
        else return false;
    }
    while (*pattern == '*') ++pattern;
    return !*pattern;
}
}
HOSFILE OsCreateFile(const char* name, uint32_t access, uint32_t, uint32_t disposition, uint32_t flags, uint32_t) {
    if (!(access & (OS_GENERIC_READ | OS_GENERIC_WRITE)) || disposition < 1 || disposition > 5) { failure(EINVAL); return OS_INVALID_FILE; }
    if ((disposition == 2 || disposition == 5) && !(access & OS_GENERIC_WRITE)) { failure(EINVAL); return OS_INVALID_FILE; }
    try {
        auto file = path(name);
        auto existing = wow::android::resolvePhysicalPath(file);
        if (!existing.empty() && (access & OS_GENERIC_WRITE) && !writable(fs::status(existing).permissions())) {
            failure(EACCES); return OS_INVALID_FILE;
        }
        if (!existing.empty()) file = existing;
        int mode = (access & OS_GENERIC_WRITE) ? ((access & OS_GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
        if (disposition == 1) mode |= O_CREAT | O_EXCL;
        if (disposition == 2) mode |= O_CREAT | O_TRUNC;
        if (disposition == 4) mode |= O_CREAT;
        if (disposition == 5) mode |= O_TRUNC;
        int fd;
        do { fd = open(file.c_str(), mode | O_CLOEXEC, 0600); } while (fd < 0 && errno == EINTR);
        if (fd < 0) { failure(); return OS_INVALID_FILE; }
        try {
            auto result = std::make_unique<HOSFILE__>(fd);
            if (!(flags & OS_FILE_ATTRIBUTE_NORMAL) && !OsSetFileAttributes(file.c_str(), flags)) {
                close(fd); return OS_INVALID_FILE;
            }
            return result.release();
        } catch (...) { close(fd); throw; }
    } catch (...) { failure(EINVAL); return OS_INVALID_FILE; }
}
bool OsCloseFile(HOSFILE file) {
    if (!valid(file)) return failure(EBADF);
    const int result = close(file->fd); delete file;
    return result == 0 || failure();
}
int32_t OsReadFile(HOSFILE file, void* buffer, uint32_t count, uint32_t* amount) {
    if (amount) *amount = 0;
    if (!valid(file) || !amount || (!buffer && count)) return failure(EINVAL);
    std::lock_guard<std::mutex> lock(file->mutex);
    while (*amount < count) {
        auto received = read(file->fd, static_cast<uint8_t*>(buffer) + *amount, count - *amount);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0) return failure();
        if (!received) break;
        *amount += static_cast<uint32_t>(received);
    }
    return 1;
}
int32_t OsWriteFile(HOSFILE file, const void* buffer, uint32_t count, uint32_t* amount) {
    if (amount) *amount = 0;
    if (!valid(file) || !amount || (!buffer && count)) return failure(EINVAL);
    std::lock_guard<std::mutex> lock(file->mutex);
    while (*amount < count) {
        auto sent = write(file->fd, static_cast<const uint8_t*>(buffer) + *amount, count - *amount);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return failure(sent < 0 ? errno : EIO);
        *amount += static_cast<uint32_t>(sent);
    }
    return 1;
}
uint64_t OsGetFileSize(HOSFILE file) {
    if (!valid(file)) { failure(EBADF); return UINT64_MAX; }
    struct stat info{};
    if (fstat(file->fd, &info)) { failure(); return UINT64_MAX; }
    return static_cast<uint64_t>(info.st_size);
}
int32_t OsFileExists(const char* name) {
    try { auto file = wow::android::resolvePhysicalPath(path(name)); return !file.empty() && fs::is_regular_file(file); }
    catch (...) { return failure(EINVAL); }
}
int32_t OsDirectoryExists(const char* name) {
    try { auto file = wow::android::resolvePhysicalPath(path(name)); return !file.empty() && fs::is_directory(file); }
    catch (...) { return failure(EINVAL); }
}
int32_t OsCreateDirectory(const char* name, int32_t recursive) {
    try {
        const auto directory = path(name);
        if (fs::is_directory(directory)) return 1;
        return recursive ? fs::create_directories(directory) : fs::create_directory(directory);
    } catch (const fs::filesystem_error& error) { return failure(error.code().value()); }
    catch (...) { return failure(EINVAL); }
}
uint32_t OsGetFileAttributes(const char* name) {
    try { auto file = wow::android::resolvePhysicalPath(path(name)); if (file.empty()) { failure(ENOENT); return UINT32_MAX; } return attributes(file); }
    catch (...) { failure(EINVAL); return UINT32_MAX; }
}
int32_t OsSetFileAttributes(const char* name, uint32_t flags) {
    try {
        auto file = wow::android::resolvePhysicalPath(path(name));
        if (file.empty()) return failure(ENOENT);
        // POSIX cannot set a Windows hidden bit independently of the filename.
        const auto filename = file.filename().string();
        if ((flags & 2) && (filename.empty() || filename.front() != '.')) return failure(ENOTSUP);
        fs::permissions(file, flags & 1 ? (fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write) : fs::perms::owner_write,
            flags & 1 ? fs::perm_options::remove : fs::perm_options::add);
        return 1;
    } catch (const fs::filesystem_error& error) { return failure(error.code().value()); }
    catch (...) { return failure(EINVAL); }
}
int32_t OsDeleteFile(const char* name) {
    try { auto file = wow::android::resolvePhysicalPath(path(name)); if (file.empty()) return failure(ENOENT); if (fs::is_directory(file) || !writable(fs::status(file).permissions())) return failure(EACCES); return fs::remove(file); }
    catch (const fs::filesystem_error& error) { return failure(error.code().value()); }
    catch (...) { return failure(EINVAL); }
}
int32_t OsRemoveDirectory(const char* name) {
    try { auto file = wow::android::resolvePhysicalPath(path(name)); if (file.empty()) return failure(ENOENT); if (!fs::is_directory(file)) return failure(ENOTDIR); return fs::remove(file); }
    catch (const fs::filesystem_error& error) { return failure(error.code().value()); }
    catch (...) { return failure(EINVAL); }
}
int32_t OsRemoveDirectoryRecurse(const char* name, uint32_t flags) {
    try {
        auto directory = wow::android::resolvePhysicalPath(path(name));
        if (directory.empty()) return failure(ENOENT);
        if (!fs::is_directory(directory)) return failure(ENOTDIR);
        if (directory == directory.root_path()) return failure(EACCES);
        if (fs::is_symlink(fs::symlink_status(directory))) return fs::remove(directory);
        if (!writable(fs::status(directory).permissions())) {
            if (!(flags & 1)) return failure(EACCES);
            fs::permissions(directory, fs::perms::owner_write | fs::perms::owner_exec, fs::perm_options::add);
        }
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (entry.is_symlink()) continue;
            if (!writable(entry.status().permissions())) {
                if (!(flags & 1)) return failure(EACCES);
                fs::permissions(entry.path(), fs::perms::owner_write, fs::perm_options::add);
            }
        }
        fs::remove_all(directory); return 1;
    } catch (const fs::filesystem_error& error) { return failure(error.code().value()); }
    catch (...) { return failure(EINVAL); }
}
int32_t OsCopyFile(const char* from, const char* to, int32_t failIfExists) {
    try {
        auto source = wow::android::resolvePhysicalPath(path(from));
        if (source.empty()) return failure(ENOENT);
        return fs::copy_file(source, path(to), failIfExists ? fs::copy_options::none : fs::copy_options::overwrite_existing);
    } catch (const fs::filesystem_error& error) { return failure(error.code().value()); }
    catch (...) { return failure(EINVAL); }
}
int32_t OsMoveFile(const char* from, const char* to) {
    try {
        auto source = wow::android::resolvePhysicalPath(path(from));
        if (source.empty()) return failure(ENOENT);
        if (!wow::android::resolvePhysicalPath(path(to)).empty()) return failure(EEXIST);
        fs::rename(source, path(to)); return 1;
    } catch (const fs::filesystem_error& error) { return failure(error.code().value()); }
    catch (...) { return failure(EINVAL); }
}
int32_t OsFileList(const char* name, const char* pattern, OsFileListCallback callback, void* context, int32_t includeHidden) {
    if (!pattern || !callback) return failure(EINVAL);
    try {
        const auto directory = wow::android::resolvePhysicalPath(path(name));
        if (directory.empty()) return failure(ENOENT);
        if (!std::strcmp(pattern, "*.*")) pattern = "*";
        for (const auto& entry : fs::directory_iterator(directory)) {
            const auto filename = entry.path().filename().string();
            if ((!includeHidden && filename.front() == '.') || !matches(filename.c_str(), pattern)) continue;
            if (filename.size() >= sizeof(OS_FILE_DATA::fileName)) return failure(ENAMETOOLONG);
            OS_FILE_DATA data{}; std::memcpy(data.fileName, filename.c_str(), filename.size() + 1);
            data.flags = attributes(entry.path());
            data.size = entry.is_regular_file() ? static_cast<uint32_t>(entry.file_size()) : 0;
            if (callback(data, context)) break;
        }
        return 1;
    } catch (const fs::filesystem_error& error) { return failure(error.code().value()); }
    catch (...) { return failure(EINVAL); }
}
bool OsSetCurrentDirectory(const char* name) {
    try { auto directory = wow::android::resolvePhysicalPath(path(name)); if (directory.empty()) return failure(ENOENT); fs::current_path(directory); return true; }
    catch (const fs::filesystem_error& error) { return failure(error.code().value()); }
    catch (...) { return failure(EINVAL); }
}
int32_t OsGetCurrentDirectory(uint32_t size, char* buffer) {
    try {
        const auto directory = fs::current_path().string();
        if (!buffer || directory.size() >= size) return failure(EINVAL);
        std::memcpy(buffer, directory.c_str(), directory.size() + 1); return directory.size();
    } catch (...) { return failure(EINVAL); }
}
char* OsFileGetDownloadFolder() {
    static auto directory = fs::current_path().string(); return directory.data();
}
