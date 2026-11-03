#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/Storm/H/SCritSect.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <malloc.h>
#include <cstdio>
#include <cstdarg>

// Portable implementations of the Storm memory/locking contracts. Kept out of
// the Windows target; no fake Win32 handles or global operator-new override.
namespace {
std::atomic<bool> exitOnFailure{true};
thread_local std::uint32_t lastError = 0;
void* failure() {
    lastError = STORM_ERROR_NOT_ENOUGH_MEMORY;
    if (exitOnFailure.load()) std::abort();
    return nullptr;
}
}

void STORMAPI SMemSetDebugFlags(uint32_t flags, uint8_t mask) {
    if (mask & SMEM_DEBUG_NOEXITONFAILURE)
        exitOnFailure.store((flags & SMEM_DEBUG_NOEXITONFAILURE) == 0);
}
// Storm blocks are plain malloc blocks: the original client's global operator new/delete
// are Storm's, so decompiled code mixes SMemAlloc/placement-new with delete (and new with
// SMemFree). Keeping both on malloc/free makes every pairing valid.
void* STORMAPI SMemAlloc(size_t bytes, const char*, int32_t, uint32_t flags) {
    void* result = std::malloc(bytes ? bytes : 1);
    if (!result) return failure();
    if (flags & SMEM_FLAG_ZEROMEMORY) std::memset(result, 0, bytes);
    return result;
}
int32_t STORMAPI SMemFree(void* pointer, const char*, int32_t, uint32_t) {
    std::free(pointer);
    return 1;
}
void* STORMAPI SMemReAlloc(void* pointer, size_t bytes, const char* file, int32_t line, uint32_t flags) {
    if (flags == SMEM_REALLOC_NULL) return nullptr;
    if (!pointer) return SMemAlloc(bytes, file, line, flags);
    if (flags & SMEM_FLAG_NOREALLOC) return nullptr;
    const size_t oldSize = malloc_usable_size(pointer);
    void* result = std::realloc(pointer, bytes ? bytes : 1);
    if (!result) return failure();
    if ((flags & SMEM_FLAG_ZEROMEMORY) && bytes > oldSize)
        std::memset(static_cast<unsigned char*>(result) + oldSize, 0, bytes - oldSize);
    return result;
}
void STORMAPI SMemInitialize() {}
int32_t STORMAPI SMemDestroy() { return 1; }
// Like upstream, this does not attempt to inspect arbitrary pointer validity.
int32_t STORMAPI SMemIsValidPointer(void*, size_t, uint32_t) { return 1; }
void STORMAPI SErrSetLastError(uint32_t error) { lastError = error; }
uint32_t STORMAPI SErrGetLastError() { return lastError; }

CCritSect::CCritSect() = default;
CCritSect::~CCritSect() = default;
void CCritSect::Enter() { m_critsect.lock(); }
void CCritSect::Enter(int32_t) { Enter(); }
void CCritSect::Leave() { m_critsect.unlock(); }
void CCritSect::Leave(int32_t) { Leave(); }

#ifndef NDEBUG
void DebugLog(const char* format, ...) {
    va_list arguments; va_start(arguments, format);
    std::vfprintf(stderr, format, arguments); std::fputc('\n', stderr); va_end(arguments);
}
void DebugLog(int32_t, const char* format, ...) {
    va_list arguments; va_start(arguments, format);
    std::vfprintf(stderr, format, arguments); std::fputc('\n', stderr); va_end(arguments);
}
void DebugValidate(const char* expression) { std::fprintf(stderr, "Validation: %s\n", expression); }
#endif
