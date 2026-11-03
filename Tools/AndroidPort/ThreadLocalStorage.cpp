#include "Engine/Source/BlizzardCore/Source/System/Thread/ThreadLocalStorage.h"
#include <cerrno>
#include <climits>
#include <limits.h>
#include <mutex>
#include <pthread.h>
#include <utility>
#include <vector>

namespace {
struct Slots {
    std::mutex mutex;
    std::vector<std::pair<pthread_key_t, Blizzard::Thread::TLSDestructor>> entries;
};
Slots& slots() {
    // Keys are process lifetime resources, including during external-thread exit.
    static Slots* registry = new Slots;
    return *registry;
}
}
namespace System_Thread {
Blizzard::Thread::TLSSlot* s_slotList[128]{};
uint32_t s_slotListUsed = 0;
bool AllocateTLSSlot(Blizzard::Thread::TLSSlot* slot, Blizzard::Thread::TLSDestructor destructor) {
    if (!slot) { errno = EINVAL; return false; }
    auto& registry = slots();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (__atomic_load_n(&slot->m_key, __ATOMIC_ACQUIRE)) return true;
    if (registry.entries.size() == 128) { errno = EAGAIN; return false; }
    pthread_key_t key;
    const int result = pthread_key_create(&key, destructor);
    if (result) { errno = result; return false; }
    if (uint64_t(key) >= UINT32_MAX) { pthread_key_delete(key); errno = EOVERFLOW; return false; }
    registry.entries.emplace_back(key, destructor);
    s_slotList[s_slotListUsed++] = slot;
    slot->m_destructor = destructor;
    // Native key zero is valid; engine zero means not allocated yet.
    __atomic_store_n(&slot->m_key, uint32_t(key) + 1, __ATOMIC_RELEASE);
    return true;
}
bool AllocateLocalStorage(Blizzard::Thread::TLSSlot* slot, Blizzard::Thread::TLSDestructor destructor) {
    return AllocateTLSSlot(slot, destructor);
}
void InternalSetLocalStorage(const Blizzard::Thread::TLSSlot* slot, const void* value) {
    if (!slot) { errno = EINVAL; return; }
    const uint32_t key = __atomic_load_n(&slot->m_key, __ATOMIC_ACQUIRE);
    if (!key) { errno = EINVAL; return; }
    const int result = pthread_setspecific(pthread_key_t(key - 1), value);
    if (result) errno = result;
}
void RunTLSDestructors() {
    for (int pass = 0; pass < PTHREAD_DESTRUCTOR_ITERATIONS; ++pass) {
        std::vector<std::pair<pthread_key_t, Blizzard::Thread::TLSDestructor>> entries;
        {
            auto& registry = slots();
            std::lock_guard<std::mutex> lock(registry.mutex);
            entries = registry.entries;
        }
        bool any = false;
        for (const auto& entry : entries) {
            if (!entry.second) continue;
            if (void* value = pthread_getspecific(entry.first)) {
                // Clear first: a destructor can register a replacement value.
                pthread_setspecific(entry.first, nullptr);
                entry.second(value);
                any = true;
            }
        }
        if (!any) break;
    }
}
void ShutdownThreadLocalStorage() { RunTLSDestructors(); }
}
void* Blizzard::Thread::RegisterLocalStorage(TLSSlot* slot, void* (*constructor)(void*), void* param, TLSDestructor destructor) {
    if (!slot || !constructor) { errno = EINVAL; return nullptr; }
    if (!__atomic_load_n(&slot->m_key, __ATOMIC_ACQUIRE) && !System_Thread::AllocateTLSSlot(slot, destructor)) return nullptr;
    const auto key = pthread_key_t(__atomic_load_n(&slot->m_key, __ATOMIC_ACQUIRE) - 1);
    if (void* value = pthread_getspecific(key)) return value;
    void* value = constructor(param);
    const int result = pthread_setspecific(key, value);
    if (result) {
        if (value && destructor) destructor(value);
        errno = result;
        return nullptr;
    }
    return value;
}
