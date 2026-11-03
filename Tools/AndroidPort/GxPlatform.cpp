#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Storm/H/PortablePrintf.h"
#include "Engine/Source/Os/Processor.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Os/Input.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace {
struct Log {
    std::mutex mutex;
    std::string path;
    bool append;
    FILE *file = nullptr;
    ~Log() {
        if (file)
            std::fclose(file);
    }
    bool open() {
        if (!file)
            file = std::fopen(path.c_str(), append ? "ab" : "wb");
        return file;
    }
};
std::mutex logsMutex;
std::map<uintptr_t, std::shared_ptr<Log>> logs;
uintptr_t nextLog = 1;
} // namespace
int32_t STORMAPI SLogCreate(const char *filename, uint32_t flags, HSLOG *output) {
    if (output)
        *output = nullptr;
    if (!output || !filename || !*filename || (flags & ~(SLOG_FLAG_OPENFILE | SLOG_FLAG_APPEND))) {
        SErrSetLastError(87);
        return 0;
    }
    auto log = std::make_shared<Log>();
    log->path = filename;
    std::replace(log->path.begin(), log->path.end(), '\\', '/');
    log->append = (flags & SLOG_FLAG_APPEND) != 0;
    if ((flags & SLOG_FLAG_OPENFILE) && !log->open()) {
        SErrSetLastError(5);
        return 0;
    }
    std::lock_guard<std::mutex> lock(logsMutex);
    const uintptr_t token = nextLog++;
    logs.emplace(token, std::move(log));
    *output = reinterpret_cast<HSLOG>(token);
    return 1;
}
void STORMAPI SLogClose(HSLOG handle) {
    std::lock_guard<std::mutex> lock(logsMutex);
    if (!logs.erase(reinterpret_cast<uintptr_t>(handle)))
        SErrSetLastError(6);
}
void SLogWrite(HSLOG handle, const char *format, ...) {
    std::shared_ptr<Log> log;
    {
        std::lock_guard<std::mutex> lock(logsMutex);
        const auto found = logs.find(reinterpret_cast<uintptr_t>(handle));
        if (found == logs.end()) {
            SErrSetLastError(6);
            return;
        }
        log = found->second;
    }
    if (!format) {
        SErrSetLastError(87);
        return;
    }
    va_list args, measure;
    va_start(args, format);
    va_copy(measure, args);
    const int length = StormPortableVsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    if (length < 0 || length > 1024 * 1024) {
        va_end(args);
        SErrSetLastError(87);
        return;
    }
    std::vector<char> text(size_t(length) + 1);
    StormPortableVsnprintf(text.data(), text.size(), format, args);
    va_end(args);
    std::lock_guard<std::mutex> lock(log->mutex);
    if (!log->open() || std::fwrite(text.data(), 1, length, log->file) != size_t(length) ||
        std::fputc('\n', log->file) == EOF || std::fflush(log->file))
        SErrSetLastError(5);
}
uint32_t OsGetProcessorFeatures() {
    // These bits name x86 instruction sets, not arbitrary SIMD availability.
    // ARM uses the portable implementations and must never advertise SSE/MMX.
    uint32_t features = 0x80000000u;
#if defined(__x86_64__) || defined(__i386__)
    unsigned a, b, c, d;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        if (d & (1u << 4))
            features |= 1;
        if (d & (1u << 23))
            features |= 2;
        if (d & (1u << 25))
            features |= 4;
        if (d & (1u << 26))
            features |= 0x10;
    }
    if (__get_cpuid(0x80000001, &a, &b, &c, &d) && (d & (1u << 31)))
        features |= 8;
#endif
    return features;
}
uint32_t OsGetNumberOfProcessors() {
    const auto count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? uint32_t(count) : 1;
}
uint64_t OsGetProcessorTicksPerSecond() { return OsGetAsyncClocksPerSecond(); }
