#include "Engine/Source/Storm/Source/SLock.h"
#include "Engine/Source/Storm/Source/SThread.h"
#include "Engine/Source/BlizzardCore/Source/System/Thread/ThreadLocalStorage.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <new>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
constexpr uint32_t failed = UINT32_MAX, timeoutResult = 258, abandonedResult = 128;
enum class Kind { Event, Semaphore, Mutex, Thread };
struct State {
    Kind kind;
    bool signaled = false, manual = false, abandoned = false;
    uint32_t count = 0, maximum = 0, depth = 0;
    std::thread::id owner;
    uint32_t tid = 0;
    std::string name;
    explicit State(Kind type) : kind(type) {}
};
using Ref = std::shared_ptr<State>;
struct Sync {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::weak_ptr<State>> threads;
};
Sync& syncHub() { static Sync* value = new Sync; return *value; }
Ref get(void* handle) { return handle ? *static_cast<Ref*>(handle) : Ref{}; }
void* handle(Ref state) { return new Ref(std::move(state)); }
struct HeldMutexes {
    std::vector<std::weak_ptr<State>> entries;
    void abandon() {
        auto& all = syncHub();
        std::lock_guard<std::mutex> lock(all.mutex);
        for (auto& weak : entries) {
            if (auto state = weak.lock(); state && state->owner == std::this_thread::get_id()) {
                state->owner = {}; state->depth = 0; state->abandoned = true;
            }
        }
        entries.clear();
        all.changed.notify_all();
    }
    ~HeldMutexes() { abandon(); }
};
thread_local HeldMutexes held;
bool ready(const Ref& state) {
    if (state->kind == Kind::Mutex) return !state->depth || state->owner == std::this_thread::get_id();
    if (state->kind == Kind::Semaphore) return state->count != 0;
    return state->signaled;
}
bool take(const Ref& state) {
    bool abandoned = false;
    if (state->kind == Kind::Mutex) {
        if (!state->depth) {
            state->owner = std::this_thread::get_id();
            if (std::none_of(held.entries.begin(), held.entries.end(), [&](const auto& weak) { return weak.lock() == state; })) held.entries.push_back(state);
        }
        ++state->depth;
        abandoned = state->abandoned; state->abandoned = false;
    } else if (state->kind == Kind::Semaphore) --state->count;
    else if (state->kind == Kind::Event && !state->manual) state->signaled = false;
    return abandoned;
}
struct Launch { Ref state; uint32_t (STORMAPI* function)(void*); void* param; };
void* run(void* opaque) {
    std::unique_ptr<Launch> launch(static_cast<Launch*>(opaque));
    {
        auto& all = syncHub();
        std::lock_guard<std::mutex> lock(all.mutex);
        launch->state->tid = uint32_t(syscall(SYS_gettid));
        all.changed.notify_all();
    }
    if (!launch->state->name.empty()) pthread_setname_np(pthread_self(), launch->state->name.c_str());
    const uint32_t result = launch->function(launch->param);
    System_Thread::RunTLSDestructors();
    held.abandon();
    {
        auto& all = syncHub();
        std::lock_guard<std::mutex> lock(all.mutex);
        launch->state->signaled = true;
        all.changed.notify_all();
    }
    return reinterpret_cast<void*>(uintptr_t(result));
}
}

SCritSect::SCritSect() = default;
SCritSect::~SCritSect() = default;
void SCritSect::Enter() { m_critsect.lock(); }
void SCritSect::Leave() { m_critsect.unlock(); }
int32_t CInitCritSect::Enter() {
    int32_t created = 0;
    while (m_spinLock.exchange(1, std::memory_order_acquire)) std::this_thread::yield();
    if (!m_critsect) { m_critsect = new (m_critsectData) CCritSect; created = 1; }
    CCritSect* section = m_critsect;
    m_spinLock.store(0, std::memory_order_release);
    section->Enter();
    return created;
}
void CInitCritSect::Leave() { m_critsect->Leave(); }
SSyncObject::SSyncObject() : m_opaqueData(nullptr) {}
SSyncObject::~SSyncObject() { Close(); }
void SSyncObject::Close() { delete static_cast<Ref*>(m_opaqueData); m_opaqueData = nullptr; }
int32_t SSyncObject::Valid() { return m_opaqueData != nullptr; }
uint32_t SSyncObject::Wait(uint32_t timeout) { SSyncObject* object = this; return WaitMultiplePtr(1, &object, 0, timeout); }
uint32_t STORMAPI SSyncObject::WaitMultiplePtr(uint32_t count, SSyncObject** objects, int32_t waitAll, uint32_t timeout) {
    if (!objects || !count || count > 64) { SErrSetLastError(87); return failed; }
    std::vector<Ref> states;
    for (uint32_t i = 0; i < count; ++i) {
        if (!objects[i] || !objects[i]->m_opaqueData) continue; // Original API skips null handles.
        auto state = get(objects[i]->m_opaqueData);
        if (std::find(states.begin(), states.end(), state) != states.end()) { SErrSetLastError(87); return failed; }
        states.push_back(std::move(state));
    }
    if (states.empty()) { SErrSetLastError(87); return failed; }
    auto& all = syncHub();
    std::unique_lock<std::mutex> lock(all.mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    for (;;) {
        if (waitAll) {
            if (std::all_of(states.begin(), states.end(), ready)) {
                uint32_t result = 0;
                for (size_t i = 0; i < states.size(); ++i) if (take(states[i]) && !result) result = abandonedResult + i;
                return result;
            }
        } else {
            for (size_t i = 0; i < states.size(); ++i) if (ready(states[i])) return take(states[i]) ? abandonedResult + i : uint32_t(i);
        }
        if (!timeout) return timeoutResult;
        if (timeout == UINT32_MAX) all.changed.wait(lock);
        else if (all.changed.wait_until(lock, deadline) == std::cv_status::timeout) {
            // Recheck once with zero wait: signal may have raced the timeout.
            timeout = 0;
        }
    }
}
SEvent::SEvent(int32_t manualReset, int32_t initialState) {
    auto state = std::make_shared<State>(Kind::Event);
    state->manual = manualReset != 0; state->signaled = initialState != 0;
    m_opaqueData = handle(std::move(state));
}
int32_t SEvent::Set() {
    auto state = get(m_opaqueData); if (!state) return 0;
    auto& all = syncHub(); std::lock_guard<std::mutex> lock(all.mutex);
    state->signaled = true; all.changed.notify_all(); return 1;
}
int32_t SEvent::Reset() {
    auto state = get(m_opaqueData); if (!state) return 0;
    std::lock_guard<std::mutex> lock(syncHub().mutex); state->signaled = false; return 1;
}
SSemaphore::SSemaphore(int32_t initialCount, int32_t maximumCount) {
    if (maximumCount <= 0 || initialCount < 0 || initialCount > maximumCount) { SErrSetLastError(87); return; }
    auto state = std::make_shared<State>(Kind::Semaphore);
    state->count = initialCount; state->maximum = maximumCount; m_opaqueData = handle(std::move(state));
}
int32_t SSemaphore::Signal(uint32_t count) {
    auto state = get(m_opaqueData); if (!state || !count) return 0;
    auto& all = syncHub(); std::lock_guard<std::mutex> lock(all.mutex);
    if (count > state->maximum - state->count) { SErrSetLastError(298); return 0; }
    state->count += count; all.changed.notify_all(); return 1;
}
SMutex::SMutex() = default;
void SMutex::Create(int32_t initialOwner, const char* name) {
    Close();
    if (name && *name) { SErrSetLastError(50); return; } // Cross-process names need a different primitive.
    auto state = std::make_shared<State>(Kind::Mutex);
    m_opaqueData = handle(state);
    if (initialOwner) { std::lock_guard<std::mutex> lock(syncHub().mutex); take(state); }
}
int32_t SMutex::Release() {
    auto state = get(m_opaqueData); if (!state) return 0;
    auto& all = syncHub(); std::lock_guard<std::mutex> lock(all.mutex);
    if (!state->depth || state->owner != std::this_thread::get_id()) { SErrSetLastError(288); return 0; }
    if (!--state->depth) { state->owner = {}; all.changed.notify_all(); }
    return 1;
}
void* SCreateThread(uint32_t stackSize, uint32_t (STORMAPI* function)(void*), void* param, uint32_t flags, uint32_t* threadId, char* name) {
    if (!function || flags) { SErrSetLastError(flags ? 50 : 87); return nullptr; }
    auto state = std::make_shared<State>(Kind::Thread);
    state->name = name ? std::string(name).substr(0, 15) : "";
    auto launch = std::make_unique<Launch>(Launch{state, function, param});
    auto output = std::make_unique<Ref>(state);
    pthread_attr_t attributes;
    int error = pthread_attr_init(&attributes);
    if (error) { SErrSetLastError(error); return nullptr; }
    error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    if (!error && stackSize) error = pthread_attr_setstacksize(&attributes, std::max(size_t(stackSize), size_t(PTHREAD_STACK_MIN)));
    pthread_t thread;
    if (!error) error = pthread_create(&thread, &attributes, run, launch.get());
    pthread_attr_destroy(&attributes);
    if (error) { SErrSetLastError(error); return nullptr; }
    launch.release();
    auto& all = syncHub(); std::unique_lock<std::mutex> lock(all.mutex);
    all.changed.wait(lock, [&] { return state->tid != 0; });
    all.threads.erase(std::remove_if(all.threads.begin(), all.threads.end(), [](const auto& weak) { return weak.expired(); }), all.threads.end());
    all.threads.push_back(state);
    if (threadId) *threadId = state->tid;
    return output.release();
}
void* STORMAPI SCreateThread(uint32_t (STORMAPI* function)(void*), void* param, uint32_t* threadId, void*, char* name, uint32_t stackSize) {
    return SCreateThread(stackSize, function, param, 0, threadId, name);
}
int32_t SThread::Create(uint32_t (STORMAPI* function)(void*), void* param, SThread& thread, char* name, uint32_t stackSize) {
    if (thread.Valid()) { SErrSetLastError(87); return 0; }
    thread.m_opaqueData = SCreateThread(stackSize, function, param, 0, nullptr, name);
    return thread.Valid();
}
int32_t SThread::SetPriority(int32_t priority) {
    static const int levels[] = {10, 5, 0, -5, -10, -20};
    auto state = get(m_opaqueData);
    if (!state || priority < 0 || priority >= 6) { SErrSetLastError(87); return 0; }
    std::lock_guard<std::mutex> lock(syncHub().mutex);
    if (state->signaled) { SErrSetLastError(6); return 0; }
    const int result = setpriority(PRIO_PROCESS, state->tid, levels[priority]);
    if (result) SErrSetLastError(errno == EACCES || errno == EPERM ? 5 : 87);
    return result == 0;
}
uint32_t STORMAPI SGetCurrentThreadId() { return uint32_t(syscall(SYS_gettid)); }
uint32_t STORMAPI SGetCurrentProcessId() { return uint32_t(getpid()); }
int32_t STORMAPI SGetCurrentThreadPriority() {
    errno = 0; const int result = getpriority(PRIO_PROCESS, 0);
    if (errno) SErrSetLastError(87);
    return -result;
}
int32_t STORMAPI SSetCurrentThreadPriority(int32_t priority) {
    const int result = setpriority(PRIO_PROCESS, 0, -std::clamp(priority, -15, 15));
    if (result) SErrSetLastError(5);
    return result == 0;
}
int32_t STORMAPI SGetThreadName(uint32_t tid, char* buffer, int32_t length) {
    if (!buffer || length <= 0) return 0;
    *buffer = 0;
    std::lock_guard<std::mutex> lock(syncHub().mutex);
    for (auto& weak : syncHub().threads) if (auto state = weak.lock(); state && state->tid == tid && !state->signaled) {
        SStrCopy(buffer, state->name.c_str(), length); return 1;
    }
    return 0;
}

namespace {
struct RWState {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread::id writer;
    unsigned writes = 0, waitingWriters = 0, readers = 0;
    std::vector<std::pair<std::thread::id, unsigned>> readerDepth;
};
}
CSRWLock::CSRWLock() { m_opaqueData.state = new RWState; }
CSRWLock::~CSRWLock() { delete static_cast<RWState*>(m_opaqueData.state); }
void CSRWLock::Enter(int32_t writing) {
    auto& state = *static_cast<RWState*>(m_opaqueData.state);
    const auto self = std::this_thread::get_id();
    std::unique_lock<std::mutex> lock(state.mutex);
    if (writing) {
        if (state.writer == self) { ++state.writes; return; }
        ++state.waitingWriters;
        state.changed.wait(lock, [&] { return !state.writes && !state.readers; });
        --state.waitingWriters; state.writer = self; state.writes = 1;
    } else {
        auto find = [&] { return std::find_if(state.readerDepth.begin(), state.readerDepth.end(), [&](const auto& entry) { return entry.first == self; }); };
        if (auto entry = find(); entry != state.readerDepth.end()) { ++entry->second; ++state.readers; return; }
        state.changed.wait(lock, [&] { return state.writer == self || (!state.writes && !state.waitingWriters); });
        state.readerDepth.emplace_back(self, 1); ++state.readers;
    }
}
void CSRWLock::Leave(int32_t writing) {
    auto& state = *static_cast<RWState*>(m_opaqueData.state);
    std::lock_guard<std::mutex> lock(state.mutex);
    if (writing) {
        if (state.writer != std::this_thread::get_id() || !state.writes) std::abort();
        if (!--state.writes) state.writer = {};
    } else {
        auto entry = std::find_if(state.readerDepth.begin(), state.readerDepth.end(), [&](const auto& value) { return value.first == std::this_thread::get_id(); });
        if (entry == state.readerDepth.end()) std::abort();
        --state.readers;
        if (!--entry->second) state.readerDepth.erase(entry);
    }
    state.changed.notify_all();
}
