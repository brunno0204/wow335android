#include <cstddef>
#include <cstdint>
#if !defined(_WIN32)
#include <stdexcept>
#endif
#include "Engine/Source/Base/Prop.h"
#include "Engine/Source/Event/EvtContext.h"
#include "Engine/Source/Os/OsCall.h"
#include "Engine/Source/Os/TimeManager.h"

// VERIFY: 8a197959 manual
// OFFSET: 0x0047E680
EvtContext::EvtContext(uint32_t flags, uint32_t idleTime, uint32_t schedWeight, void* callContext, int32_t startWatchdog) : m_timerQueue(offsetof(EvtTimer, m_targetTime)) {
#if !defined(_WIN32)
    if (callContext || startWatchdog) throw std::invalid_argument("Native Event call-stack/watchdog context is unported");
#endif
    this->m_currTime = 0;
    this->m_schedState = SCHEDSTATE_ACTIVE;
    this->m_schedLastIdle = OsGetAsyncTimeMs();
    this->m_schedFlags = flags;
    this->m_schedIdleTime = idleTime;
    this->m_schedInitialIdleTime = idleTime;
    this->m_schedWeight = schedWeight;
    this->m_schedSmoothWeight = schedWeight;
    this->m_schedRebalance = 0;
    this->m_queueSyncButtonState = 0;
    this->m_propContext = PropCreateContext();
    this->m_callContext = callContext;
    this->m_startWatchdog = startWatchdog;
}

// VERIFY: a33f23a6 manual
// OFFSET: [0x0047F6C0, 0x0047F920]
EvtContext::~EvtContext() {
    for (uint32_t i = 0; i < EVENTIDS; i++) {
        this->m_queueHandlerList[i].Clear();
    }

    this->m_queueMessageList.Clear();
    this->m_queueSyncKeyDownList.Clear();

    for (uint32_t i = 1; i < this->m_timerIdTable.m_allocArray.Count(); i++) {
#if defined(_WIN32)
        delete this->m_timerIdTable.m_allocArray[i];
#else
        auto *timer = this->m_timerIdTable.m_allocArray[i];
        if (timer) { timer->~EvtTimer(); SMemFree(timer, __FILE__, __LINE__, 0); }
#endif
    }

    PropDeleteContext(this->m_propContext);
#if defined(_WIN32)
    OsCallDestroyContext(this->m_callContext);
#endif
}
