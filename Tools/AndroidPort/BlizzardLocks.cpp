#include "Engine/Source/BlizzardCore/Source/System/Lock/Lock.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace Blizzard::Lock {
struct EventState {
    std::mutex mutex;
    std::condition_variable changed;
    bool signaled = false;
};
namespace { void checked(int result) { if (result) std::abort(); } }
int32_t Atomic::Increment() { return __atomic_add_fetch(&m_value, 1, __ATOMIC_SEQ_CST); }
int32_t Atomic::Decrement() { return __atomic_sub_fetch(&m_value, 1, __ATOMIC_SEQ_CST); }
void MutexCreate(Mutex& mutex) {
    pthread_mutexattr_t attributes;
    checked(pthread_mutexattr_init(&attributes));
    checked(pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE));
    checked(pthread_mutex_init(&mutex, &attributes));
    checked(pthread_mutexattr_destroy(&attributes));
}
void MutexDestroy(Mutex& mutex) { checked(pthread_mutex_destroy(&mutex)); }
void MutexEnter(Mutex& mutex) { checked(pthread_mutex_lock(&mutex)); }
void MutexLeave(Mutex& mutex) { checked(pthread_mutex_unlock(&mutex)); }
RMutex::RMutex() { MutexCreate(m_mutex); }
RMutex::~RMutex() { MutexDestroy(m_mutex); }
ScopedMutex::ScopedMutex(Mutex& mutex) : m_mutex(&mutex) { MutexEnter(mutex); }
ScopedMutex::~ScopedMutex() { MutexLeave(*m_mutex); }
void EventCreate(Event& event) { event = new EventState; }
void EventDestroy(Event& event) { delete event; event = nullptr; }
void EventSet(Event& event) {
    std::lock_guard<std::mutex> guard(event->mutex);
    event->signaled = true;
    event->changed.notify_one();
}
Event* EventWait(uint32_t timeout, Event& event) {
    std::unique_lock<std::mutex> guard(event->mutex);
    const auto ready = [&] { return event->signaled; };
    if (timeout == UINT32_MAX) event->changed.wait(guard, ready);
    else if (!event->changed.wait_for(guard, std::chrono::milliseconds(timeout), ready)) return nullptr;
    event->signaled = false;
    return &event;
}
void EventWaitForever(Event& event) { EventWait(UINT32_MAX, event); }
EventObject::EventObject() { EventCreate(m_event); }
EventObject::~EventObject() { EventDestroy(m_event); }
}
void Blizzard::Process::Sleep(uint32_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
