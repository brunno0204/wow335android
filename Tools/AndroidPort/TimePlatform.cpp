#include "Engine/Source/Os/TimeManager.h"
#include <chrono>
#include <thread>

// The profiling VM needs a monotonic clock and a matching conversion frequency.
uint64_t OsGetAsyncTimeClocks() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
int64_t OsGetAsyncClocksPerSecond() { return 1000000000; }
uint32_t OsGetAsyncTimeMs() { return static_cast<uint32_t>(OsGetAsyncTimeClocks() / 1000000); }
void OsSleep(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
