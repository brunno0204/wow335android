#pragma once
namespace wow::android {
// Android owns frame pacing/EGL. Run Event callbacks on that same owner thread.
bool pumpEvents();
bool eventsInitialized();
// Host must have stopped the previous GL thread before transferring ownership.
void rebindEventsToCurrentThread();
} // namespace wow::android
