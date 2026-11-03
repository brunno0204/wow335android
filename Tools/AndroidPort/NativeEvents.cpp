#include "NativeEvents.h"
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/Event/EvtContext.h"
#include "Engine/Source/Event/EvtInput.h"
#include "Engine/Source/Event/EvtQueue.h"
#include "Engine/Source/Event/EvtTimer.h"
#include "Engine/Source/Base/Prop.h"
#include "Engine/Source/Os/TimeManager.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Context = std::shared_ptr<EvtContext>;
std::mutex contextsMutex;
std::map<uintptr_t, Context> contexts;
uintptr_t nextContext = 1;
std::thread::id owner;
bool initialized = false, pumping = false, shutdownRequested = false;
void requireOwner() {
    if (!initialized || std::this_thread::get_id() != owner)
        throw std::logic_error("Event operation requires the initialized Android frame thread");
}
Context find(HEVENTCONTEXT handle = nullptr) {
    if (!handle)
        handle = static_cast<HEVENTCONTEXT>(PropGet(PROP_EVENTCONTEXT));
    std::lock_guard<std::mutex> lock(contextsMutex);
    auto i = contexts.find(reinterpret_cast<uintptr_t>(handle));
    if (i == contexts.end()) {
        SErrSetLastError(6);
        return {};
    }
    return i->second;
}
struct ContextLock {
    SCritSect &mutex;
    explicit ContextLock(EvtContext &context) : mutex(context.m_critsect) { mutex.Enter(); }
    ~ContextLock() { mutex.Leave(); }
};
struct Select {
    HPROPCONTEXT previous;
    Select(EvtContext &context) : previous(PropGetSelectedContext()) {
        PropSelectContext(context.m_propContext);
    }
    ~Select() { PropSelectContext(previous); }
};
void eventId(EVENTID id) {
    if (id < EVENT_ID_0 || id >= EVENTIDS)
        throw std::invalid_argument("Invalid original Event ID");
}
void closeContext(EvtContext &context) {
    if (context.m_schedState == SCHEDSTATE_DESTROYED)
        return;
    context.m_critsect.Enter();
    context.m_schedState = SCHEDSTATE_CLOSED;
    context.m_critsect.Leave();
    IEvtQueueDispatch(&context, EVENT_ID_3, nullptr);
    context.m_critsect.Enter();
    context.m_schedState = SCHEDSTATE_DESTROYED;
    context.m_critsect.Leave();
    IEvtQueueDispatchAll(&context);
    IEvtQueueDispatch(&context, EVENT_ID_DESTROY, nullptr);
}
} // namespace
// Original queue only needs this scheduler query. Scheduling is host-frame driven.
int32_t IEvtSchedulerIsContextInteractive(EvtContext *context) { return (context->m_schedFlags >> 1) & 1; }
int32_t STORMAPI SErrIsDisplayingError() { return 0; } // Native errors are reported, no desktop modal dialog.
void EventInitialize(uint32_t threadCount, int32_t netServer) {
    if (initialized)
        throw std::logic_error("Native Event runtime already initialized");
    if (threadCount > 1 || netServer)
        throw std::invalid_argument(
            "Native Event runtime supports one host frame thread, no network scheduler");
    owner = std::this_thread::get_id();
    PropInitialize();
    IEvtQueueInitialize();
    IEvtInputInitialize();
    IEvtInputSetConfirmCloseCallback(nullptr, nullptr);
    Input::s_mouseMode = MOUSE_MODE_NORMAL;
    Input::s_mouseHoldButton = Input::s_metaKeyState = Input::s_buttonState = 0;
    Input::s_boundingRect = CRect{};
    shutdownRequested = false;
    initialized = true;
}
HEVENTCONTEXT EventCreateContextEx(int32_t interactive, EVENTHANDLERFUNC init, EVENTHANDLERFUNC destroy,
                                   uint32_t idle, uint32_t debugFlags) {
    requireOwner();
    if (debugFlags)
        throw std::invalid_argument("Native Event call-stack/watchdog contexts are unported");
    std::lock_guard<std::mutex> lock(contextsMutex);
    if (contexts.size() >= 64)
        throw std::runtime_error("Native Event context limit reached");
    if (interactive)
        for (const auto &entry : contexts)
            if ((entry.second->m_schedFlags & 2) && entry.second->m_schedState == SCHEDSTATE_ACTIVE)
                throw std::logic_error("Only one interactive input context may own the Android surface");
    auto context = Context(new EvtContext(interactive ? 2 : 0, std::max(1u, idle), 1, nullptr, 0));
    const uintptr_t id = nextContext++;
    context->m_id = uint32_t(id);
    auto handle = reinterpret_cast<HEVENTCONTEXT>(id);
    auto previous = PropGetSelectedContext();
    PropSelectContext(context->m_propContext);
    PropSet(PROP_EVENTCONTEXT, handle);
    PropSelectContext(previous);
    if (init)
        IEvtQueueRegister(context.get(), EVENT_ID_INITIALIZE, init, nullptr, 1000);
    if (destroy)
        IEvtQueueRegister(context.get(), EVENT_ID_DESTROY, destroy, nullptr, 1000);
    contexts.emplace(id, context);
    return handle;
}
void EventDestroy() {
    requireOwner();
    if (pumping)
        throw std::logic_error("Destroy Event runtime after the current frame callbacks finish");
    std::vector<Context> snapshot;
    {
        std::lock_guard<std::mutex> lock(contextsMutex);
        for (auto &e : contexts)
            snapshot.push_back(e.second);
    }
    for (auto &context : snapshot) {
        Select select(*context);
        closeContext(*context);
    }
    {
        std::lock_guard<std::mutex> lock(contextsMutex);
        contexts.clear();
    }
    snapshot.clear();
    PropSelectContext(nullptr);
    IEvtInputDestroy();
    IEvtQueueDestroy();
    PropDestroy();
    initialized = false;
}
HEVENTCONTEXT EventGetCurrentContext() {
    requireOwner();
    return static_cast<HEVENTCONTEXT>(PropGet(PROP_EVENTCONTEXT));
}
void EventDoMessageLoop() {
    throw std::logic_error("Android GLSurfaceView owns the message/frame loop; call pumpEvents per frame");
}
void EventInitiateShutdown() {
    requireOwner();
    shutdownRequested = true;
}
void EventSetConfirmCloseCallback(int32_t (*callback)(void *), void *param) {
    requireOwner();
    IEvtInputSetConfirmCloseCallback(callback, param);
}
void EventRegisterEx(EVENTID id, EVENTHANDLERFUNC handler, void *param, float priority) {
    requireOwner();
    eventId(id);
    if (!handler || !std::isfinite(priority))
        throw std::invalid_argument("Invalid Event handler/priority");
    if (auto context = find())
        IEvtQueueRegister(context.get(), id, handler, param, priority);
}
void EventRegister(EVENTID id, EVENTHANDLERFUNC handler) { EventRegisterEx(id, handler, nullptr, 0); }
void EventUnregisterEx(EVENTID id, EVENTHANDLERFUNC handler, void *param, uint32_t flags) {
    requireOwner();
    if (flags & 1)
        eventId(id);
    if (auto context = find())
        IEvtQueueUnregister(context.get(), id, handler, param, flags);
}
void EventUnregister(EVENTID id, EVENTHANDLERFUNC handler) {
    EventUnregisterEx(id, handler, nullptr, 0xffffffff);
}
int32_t EventQueuePost(HEVENTCONTEXT handle, EVENTID id, const void *data, uint32_t bytes) {
    eventId(id);
    if (!handle)
        requireOwner(); // A worker must name an explicit context.
    size_t required = 0;
    switch (id) {
    case EVENT_ID_CHAR:
        required = sizeof(EVENT_DATA_CHAR);
        break;
    case EVENT_ID_FOCUS:
        required = sizeof(EVENT_DATA_FOCUS);
        break;
    case EVENT_ID_KEYDOWN:
    case EVENT_ID_KEYUP:
    case EVENT_ID_KEYDOWN_REPEATING:
        required = sizeof(EVENT_DATA_KEY);
        break;
    case EVENT_ID_MOUSEDOWN:
    case EVENT_ID_MOUSEUP:
    case EVENT_ID_MOUSEMOVE:
    case EVENT_ID_MOUSEMOVE_RELATIVE:
    case EVENT_ID_MOUSEWHEEL:
    case EVENT_ID_MOUSEMODE_CHANGED:
        required = sizeof(EVENT_DATA_MOUSE);
        break;
    case EVENT_ID_SIZE:
        required = sizeof(EVENT_DATA_SIZE);
        break;
    case EVENT_ID_UPDATE:
        required = sizeof(EVENT_DATA_UPDATE);
        break;
    case EVENT_ID_IDLE:
        required = sizeof(EVENT_DATA_IDLE);
        break;
    default:
        break;
    }
    if (bytes < required)
        throw std::invalid_argument("Truncated original Event payload");
    if (bytes > 1024 * 1024 || (bytes && !data))
        throw std::invalid_argument("Invalid/oversized Event payload");
    auto context = find(handle);
    if (!context)
        return 0;
    ContextLock lock(*context);
    const bool active = context->m_schedState != SCHEDSTATE_DESTROYED &&
                        context->m_nativePostedCount < 4096 &&
                        uint64_t(context->m_nativePostedBytes) + bytes <= 16 * 1024 * 1024;
    if (active)
        IEvtQueuePost(context.get(), id, data, bytes);
    return active;
}
void EventPostCloseEx(HEVENTCONTEXT handle) {
    requireOwner();
    if (auto context = find(handle)) {
        context->m_critsect.Enter();
        context->m_schedState = SCHEDSTATE_CLOSED;
        context->m_critsect.Leave();
    }
}
void EventPostClose() { EventPostCloseEx(nullptr); }
int32_t EventIsKeyDown(KEY key) {
    requireOwner();
    auto c = find();
    return c ? IEvtQueueCheckSyncKeyState(c.get(), key) : 0;
}
int32_t EventIsButtonDown(MOUSEBUTTON button) {
    requireOwner();
    auto c = find();
    return c ? IEvtQueueCheckSyncMouseState(c.get(), button) : 0;
}
void EventSetMouseMode(MOUSEMODE mode, uint32_t hold) {
    requireOwner();
    if (auto c = find())
        IEvtInputSetMouseMode(c.get(), mode, hold);
}
void EventInputGetMousePosition(float *x, float *y) {
    requireOwner();
    if (!x || !y)
        throw std::invalid_argument("Null Event cursor output");
    *x = *y = 0;
    if (find())
        IEvtInputGetMousePosition(x, y);
}
int32_t EventInputProcess(HEVENTCONTEXT handle) {
    requireOwner();
    auto c = find(handle);
    if (!c)
        return 0;
    Select select(*c);
    int32_t shutdown = 0;
    const auto result = IEvtInputProcess(c.get(), &shutdown);
    if (shutdown)
        shutdownRequested = true;
    return result;
}
uint32_t EventSetTimer(uint32_t delay, EVENTHANDLERFUNC handler, void *param) {
    requireOwner();
    if (auto c = find())
        return IEvtTimerSet(c.get(), delay, handler, param, nullptr, 0, nullptr);
    return 0;
}
uint32_t EventSetTimer(uint32_t delay, EVENTHANDLERGUIDFUNC handler, uint64_t guid, void *param) {
    requireOwner();
    if (auto c = find())
        return IEvtTimerSet(c.get(), delay, nullptr, nullptr, handler, guid, param);
    return 0;
}
void EventKillTimer(uint32_t id, EVENTHANDLERFUNC handler, const char *name) {
    requireOwner();
    if (auto c = find())
        IEvtTimerKill(c.get(), id, handler, name);
}
namespace wow::android {
bool eventsInitialized() { return initialized; }
void rebindEventsToCurrentThread() {
    if (!initialized || pumping)
        throw std::logic_error("Event thread transfer requires an initialized quiescent runtime");
    owner = std::this_thread::get_id();
    const uint32_t now = OsGetAsyncTimeMs();
    std::lock_guard<std::mutex> lock(contextsMutex);
    for (auto &entry : contexts)
        entry.second->m_currTime = entry.second->m_schedLastIdle = now;
}
bool pumpEvents() {
    requireOwner();
    if (pumping)
        throw std::logic_error("Recursive Event frame pump");
    struct Guard {
        Guard() { pumping = true; }
        ~Guard() { pumping = false; }
    } guard;
    std::vector<Context> snapshot;
    {
        std::lock_guard<std::mutex> lock(contextsMutex);
        for (auto &e : contexts)
            snapshot.push_back(e.second);
    }
    const uint32_t now = OsGetAsyncTimeMs();
    bool closedInteractive = false;
    for (auto &context : snapshot) {
        Select select(*context);
        if (!(context->m_schedFlags & 1)) {
            context->m_schedFlags |= 1;
            context->m_currTime = context->m_schedLastIdle = now;
            IEvtQueueDispatch(context.get(), EVENT_ID_INITIALIZE, nullptr);
        }
        if (context->m_schedState == SCHEDSTATE_CLOSED) {
            closedInteractive |= (context->m_schedFlags & 2) != 0;
            closeContext(*context);
            continue;
        }
        if (context->m_schedState == SCHEDSTATE_DESTROYED)
            continue;
        IEvtQueueDispatchAll(context.get());
        IEvtQueueDispatch(context.get(), EVENT_ID_POLL, nullptr);
        if (context->m_schedFlags & 2) {
            int32_t shutdown = 0;
            IEvtInputProcess(context.get(), &shutdown);
            if (shutdown)
                shutdownRequested = true;
        }
        IEvtTimerDispatch(context.get());
        if (context->m_schedState != SCHEDSTATE_ACTIVE)
            continue;
        EVENT_DATA_UPDATE update{float(now - context->m_currTime) / 1000.0f, now};
        context->m_currTime = now;
        IEvtQueueDispatch(context.get(), EVENT_ID_UPDATE, &update);
        if (context->m_schedState != SCHEDSTATE_ACTIVE)
            continue;
        if (uint32_t(now - context->m_schedLastIdle) >= context->m_schedIdleTime) {
            EVENT_DATA_IDLE idle{float(now - context->m_schedLastIdle) / 1000.0f, now};
            IEvtQueueDispatch(context.get(), EVENT_ID_IDLE, &idle);
            context->m_schedLastIdle = now;
        }
        if (context->m_schedFlags & 2)
            IEvtQueueDispatch(context.get(), EVENT_ID_PAINT, nullptr);
    }
    {
        std::lock_guard<std::mutex> lock(contextsMutex);
        for (auto it = contexts.begin(); it != contexts.end();) {
            if (it->second->m_schedState == SCHEDSTATE_DESTROYED)
                it = contexts.erase(it);
            else
                ++it;
        }
    }
    if (closedInteractive) {
        std::lock_guard<std::mutex> lock(contextsMutex);
        const bool remaining = std::any_of(contexts.begin(), contexts.end(), [](const auto &entry) {
            return (entry.second->m_schedFlags & 2) && entry.second->m_schedState == SCHEDSTATE_ACTIVE;
        });
        if (!remaining)
            shutdownRequested = true;
    }
    return !shutdownRequested;
}
} // namespace wow::android
