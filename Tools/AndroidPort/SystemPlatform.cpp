#include "Engine/Source/Os/Tls.h"
#include "Engine/Source/Os/OsSecureRandom.h"
#include "Engine/Source/Os/OsLibrary.h"
#include <pthread.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <sys/syscall.h>
#include <stdexcept>

static_assert(sizeof(pthread_key_t) <= sizeof(uint32_t), "Unsupported POSIX TLS key size");
uint32_t OsTlsAlloc() {
    pthread_key_t key;
    return pthread_key_create(&key, nullptr) == 0 ? static_cast<uint32_t>(key) : UINT32_MAX;
}
void OsTlsFree(uint32_t key) { pthread_key_delete(static_cast<pthread_key_t>(key)); }
void* OsTlsGetValue(uint32_t key) { return pthread_getspecific(static_cast<pthread_key_t>(key)); }
int32_t OsTlsSetValue(uint32_t key, void* value) {
    return pthread_setspecific(static_cast<pthread_key_t>(key), value) == 0;
}

void OsSecureRandom(uint8_t *buffer, uint32_t size) {
    if (size && !buffer)
        throw std::invalid_argument("Missing random output");
    if (size > 1024 * 1024)
        throw std::length_error("Random request exceeds budget");
    while (size) {
        const auto n = ::syscall(SYS_getrandom, buffer, size, 0);
        if (n > 0) {
            buffer += n;
            size -= uint32_t(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == ENOSYS)
            break;
        throw std::runtime_error("System random generation failed");
    }
    if (!size)
        return;
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("System random source unavailable");
    struct Close {
        int fd;
        ~Close() { ::close(fd); }
    } close{fd};
    while (size) {
        const auto n = ::read(fd, buffer, size);
        if (n > 0) {
            buffer += n;
            size -= uint32_t(n);
        } else if (n < 0 && errno == EINTR)
            continue;
        else
            throw std::runtime_error("System random read failed");
    }
}
void IOsSecureRandom(uint8_t *buffer, uint32_t size) { OsSecureRandom(buffer, size); }

void* OsLoadLibrary(const char* name) { return name ? dlopen(name, RTLD_NOW | RTLD_LOCAL) : nullptr; }
void* OsGetLibraryEntryPoint(void* library, const char* name) {
    return library && name ? dlsym(library, name) : nullptr;
}
void OsFreeLibrary(void* library) { if (library) dlclose(library); }
