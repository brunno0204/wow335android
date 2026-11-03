#include <cstdlib>
#include <chrono>
#include "NativeInput.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Os/TimeManager.h"
#include "Engine/Source/Storm/H/storm.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>

namespace {
std::mutex inputMutex;
std::deque<OSEVENT> events;
std::array<bool, KEY_LAST> keys{};
uint32_t buttons = 0;
int32_t pointerX = 0, pointerY = 0;
int32_t surfaceWidth = 1, surfaceHeight = 1;
bool focused = true;
float mouseSpeed = 1.0f;
OS_MOUSE_MODE mouseMode = OS_MOUSE_MODE_NORMAL;
void (*pollProc)() = nullptr;
float cameraRemainderX = 0, cameraRemainderY = 0;
constexpr size_t maxEvents = 4096;
void resetHeld() {
    keys.fill(false);
    buttons = 0;
    cameraRemainderX = cameraRemainderY = 0;
}
void append(OSINPUT id, int32_t a = 0, int32_t b = 0, int32_t c = 0) {
    events.push_back({id, {a, b, c, static_cast<int32_t>(OsGetAsyncTimeMs())}});
}
void put(OSINPUT id, int32_t a, int32_t b, int32_t c) {
    if (events.size() >= maxEvents) {
        // Dropping an arbitrary key-up leaves movement stuck. Force a consumer
        // focus reset before admitting fresh events, and clear producer state.
        events.clear();
        resetHeld();
        append(OS_INPUT_FOCUS, 0);
        if (focused)
            append(OS_INPUT_FOCUS, 1);
    }
    if (id == OS_INPUT_FOCUS) {
        focused = a != 0;
        if (!focused)
            resetHeld();
    }
    if (!focused &&
        (id == OS_INPUT_KEY_DOWN || id == OS_INPUT_MOUSE_DOWN || id == OS_INPUT_MOUSE_MOVE_RELATIVE))
        return;
    if (id == OS_INPUT_KEY_DOWN || id == OS_INPUT_KEY_UP) {
        if (a < 0 || a >= KEY_LAST) {
            SErrSetLastError(87);
            return;
        }
        keys[size_t(a)] = id == OS_INPUT_KEY_DOWN;
    }
    if (id == OS_INPUT_MOUSE_DOWN || id == OS_INPUT_MOUSE_UP) {
        const uint32_t button = uint32_t(a);
        if (!button || button > MOUSE_BUTTON_XBUTTON28 || (button & (button - 1))) {
            SErrSetLastError(87);
            return;
        }
        if (id == OS_INPUT_MOUSE_DOWN)
            buttons |= button;
        else
            buttons &= ~button;
    }
    if (id == OS_INPUT_MOUSE_MOVE || id == OS_INPUT_MOUSE_DOWN || id == OS_INPUT_MOUSE_UP) {
        pointerX = b;
        pointerY = c;
    }
    append(id, a, b, c);
}
void key(KEY which, bool held) {
    if (keys[which] != held)
        put(held ? OS_INPUT_KEY_DOWN : OS_INPUT_KEY_UP, int32_t(which), 1, 0);
}
void cameraButton(bool held) {
    if (bool(buttons & MOUSE_BUTTON_RIGHT) != held)
        put(held ? OS_INPUT_MOUSE_DOWN : OS_INPUT_MOUSE_UP, MOUSE_BUTTON_RIGHT, pointerX, pointerY);
}
int32_t delta(float value, float &remainder) {
    // Clamp before conversion; malformed JNI values cannot overflow integers.
    const float sum = std::clamp(value, -32768.0f, 32768.0f) + remainder;
    const auto result = static_cast<int32_t>(sum);
    remainder = sum - result;
    return result;
}
} // namespace
void OsQueuePut(OSINPUT id, int32_t a, int32_t b, int32_t c, int32_t) {
    if (id < OS_INPUT_CAPTURE_CHANGED || id > OS_INPUT_SHUTDOWN) {
        SErrSetLastError(87);
        return;
    }
    if (id == OS_INPUT_STRING || id == OS_INPUT_IME) {
        // The original queue encodes string/composition pointers in 32-bit slots.
        // Android text needs a separate owned payload bridge, not truncated pointers.
        SErrSetLastError(50);
        return;
    }
    std::lock_guard<std::mutex> lock(inputMutex);
    put(id, a, b, c);
}
int32_t OsQueueGet(OSINPUT *id, int32_t *a, int32_t *b, int32_t *c, int32_t *time) {
    if (!id || !a || !b || !c || !time) {
        SErrSetLastError(87);
        return 0;
    }
    std::lock_guard<std::mutex> lock(inputMutex);
    if (events.empty())
        return 0;
    const auto event = events.front();
    events.pop_front();
    *id = event.id;
    *a = event.param[0];
    *b = event.param[1];
    *c = event.param[2];
    *time = event.param[3];
    return 1;
}
int32_t OsInputGet(OSINPUT *id, int32_t *a, int32_t *b, int32_t *c, int32_t *time) {
    void (*poll)();
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        poll = pollProc;
    }
    // Callback may itself enqueue input or change the poll function.
    if (poll)
        poll();
    return OsQueueGet(id, a, b, c, time);
}
void OsInputSetEventPollProc(void (*poll)()) {
    std::lock_guard<std::mutex> lock(inputMutex);
    pollProc = poll;
}
void OsInputInitialize() {
    std::lock_guard<std::mutex> lock(inputMutex);
    events.clear();
    resetHeld();
    focused = true;
    pointerX = pointerY = 0;
    mouseSpeed = 1;
    mouseMode = OS_MOUSE_MODE_NORMAL;
}
void OsInputDestroy() {
    std::lock_guard<std::mutex> lock(inputMutex);
    events.clear();
    resetHeld();
    focused = false;
    pollProc = nullptr;
}
int32_t OsInputIsKeyDown(KEY which) {
    std::lock_guard<std::mutex> lock(inputMutex);
    return uint32_t(which) < keys.size() && keys[which];
}
int32_t OsInputIsMouseButtonDown(MOUSEBUTTON which) {
    std::lock_guard<std::mutex> lock(inputMutex);
    return (buttons & uint32_t(which)) != 0;
}
int32_t OsGetDefaultWindowRect(tagRECT *rect) {
    if (!rect) {
        SErrSetLastError(87);
        return 0;
    }
    std::lock_guard<std::mutex> lock(inputMutex);
    *rect = {0, 0, surfaceWidth, surfaceHeight};
    return 1;
}
void OsInputSetMousePosition(int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lock(inputMutex);
    pointerX = x;
    pointerY = y;
}
void OsInputGetMousePosition(int32_t *x, int32_t *y) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (x)
        *x = pointerX;
    if (y)
        *y = pointerY;
}
void OsInputSetMouseMode(OS_MOUSE_MODE mode) {
    if (mode != OS_MOUSE_MODE_NORMAL && mode != OS_MOUSE_MODE_RELATIVE) {
        SErrSetLastError(87);
        return;
    }
    std::lock_guard<std::mutex> lock(inputMutex);
    mouseMode = mode;
}
float OsInputGetMouseSpeed() {
    std::lock_guard<std::mutex> lock(inputMutex);
    return mouseSpeed;
}
void OsInputSetMouseSpeed(float speed) {
    if (!std::isfinite(speed)) {
        SErrSetLastError(87);
        return;
    }
    std::lock_guard<std::mutex> lock(inputMutex);
    mouseSpeed = std::clamp(speed, .1f, 2.0f);
}
void OsInputSetWindowResizeLock(int32_t enabled) {
    // Embedded Android surface dimensions are owned by SurfaceView/WindowManager.
    if (enabled)
        SErrSetLastError(50);
}
void OsInputEnableSecureMode(int32_t enabled) {
    // No desktop global input hooks are installed; OS secure keyboard mode is
    // unavailable rather than being silently advertised as enabled.
    if (enabled)
        SErrSetLastError(50);
}
namespace wow::android {
void inputFocus(bool active) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!active) {
        // Emit releases as well as focus loss: consumers may cache bindings.
        for (uint32_t i = 0; i < keys.size(); ++i)
            if (keys[i])
                put(OS_INPUT_KEY_UP, i, 1, 0);
        for (uint32_t bit = 1; bit && bit <= MOUSE_BUTTON_XBUTTON28; bit <<= 1)
            if (buttons & bit)
                put(OS_INPUT_MOUSE_UP, bit, pointerX, pointerY);
    }
    put(OS_INPUT_FOCUS, active, 0, 0);
}
namespace {
float analogRight = 0, analogForward = 0;
bool freeLookOn = false, cameraHeld = false;
} // namespace
void touchAnalog(float &right, float &forward) {
    std::lock_guard<std::mutex> lock(inputMutex);
    right = focused ? analogRight : 0;
    forward = focused ? analogForward : 0;
}
void touchFreeLook(bool on) {
    std::lock_guard<std::mutex> lock(inputMutex);
    freeLookOn = on;
}
bool touchFreeLookActive() {
    std::lock_guard<std::mutex> lock(inputMutex);
    return freeLookOn && cameraHeld;
}
void touchMovement(float right, float forward) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!focused)
        return;
    if (!std::isfinite(right) || !std::isfinite(forward))
        right = forward = 0;
    analogRight = std::clamp(right, -1.f, 1.f);
    analogForward = std::clamp(forward, -1.f, 1.f);
    // Default WoW bindings: W/S forward/back and Q/E strafe. This is digital
    // movement, not analog speed; native joystick axes are not game bindings.
    constexpr float threshold = .28f;
    key(KEY_W, forward > threshold);
    key(KEY_S, forward < -threshold);
    key(KEY_Q, right < -threshold);
    key(KEY_E, right > threshold);
}
void touchCamera(bool held, float dx, float dy) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!focused)
        return;
    cameraButton(held);
    cameraHeld = held;
    if (!held) {
        cameraRemainderX = cameraRemainderY = 0;
        return;
    }
    if (!std::isfinite(dx) || !std::isfinite(dy))
        return;
    const auto x = delta(dx, cameraRemainderX), y = delta(dy, cameraRemainderY);
    if (x || y)
        put(OS_INPUT_MOUSE_MOVE_RELATIVE, 0, x, y);
}
void touchJump(bool held) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (focused)
        key(KEY_SPACE, held);
}
void touchPointer(int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (focused)
        put(OS_INPUT_MOUSE_MOVE, 0, x, y);
}
void touchTap(int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!focused)
        return;
    put(OS_INPUT_MOUSE_MOVE, 0, x, y);
    put(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_LEFT, x, y);
    put(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_LEFT, x, y);
}
// Finger as the mouse: a short tap is a left click, a long press (>= 0.5 s without moving) a right
// click (unit menus, selling, cancelling buffs), and a finger that moves holds the left button
// (dragging items, sliders, scroll bars). The press is sent once the gesture is known.
namespace {
struct Press {
    bool active = false, dragging = false;
    int32_t x = 0, y = 0;
    std::chrono::steady_clock::time_point start;
} press;
constexpr int32_t kDragSlop = 14;
constexpr auto kLongPress = std::chrono::milliseconds(500);
} // namespace
static std::chrono::steady_clock::time_point lastTouch;
static bool pointerParked = true;
void touchParkPointer() {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!focused || pointerParked || press.active || (buttons & (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT)))
        return;
    if (std::chrono::steady_clock::now() - lastTouch < std::chrono::milliseconds(4000))
        return;
    pointerParked = true;
    put(OS_INPUT_MOUSE_MOVE, 0, -4096, -4096);
}
void touchDown(int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!focused)
        return;
    lastTouch = std::chrono::steady_clock::now();
    pointerParked = false;
    put(OS_INPUT_MOUSE_MOVE, 0, x, y);
    press = Press{true, false, x, y, std::chrono::steady_clock::now()};
}
void touchMove(int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!focused)
        return;
    if (press.active && !press.dragging &&
        (std::abs(x - press.x) > kDragSlop || std::abs(y - press.y) > kDragSlop)) {
        press.dragging = true;
        put(OS_INPUT_MOUSE_MOVE, 0, press.x, press.y);
        if (!(buttons & MOUSE_BUTTON_LEFT))
            put(OS_INPUT_MOUSE_DOWN, MOUSE_BUTTON_LEFT, press.x, press.y);
    }
    put(OS_INPUT_MOUSE_MOVE, 0, x, y);
}
void touchUp(int32_t x, int32_t y) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!focused)
        return;
    lastTouch = std::chrono::steady_clock::now();
    pointerParked = false;
    const bool wasPress = press.active && !press.dragging;
    const bool longPress = wasPress && std::chrono::steady_clock::now() - press.start >= kLongPress;
    press.active = false;
    if (wasPress) {
        x = press.x;
        y = press.y;
    }
    put(OS_INPUT_MOUSE_MOVE, 0, x, y);
    if (wasPress) {
        const auto button = longPress ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_LEFT;
        put(OS_INPUT_MOUSE_DOWN, button, x, y);
        put(OS_INPUT_MOUSE_UP, button, x, y);
    } else if (buttons & MOUSE_BUTTON_LEFT)
        put(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_LEFT, x, y);
}
void touchCancel() {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!focused)
        return;
    const bool dragging = press.active && press.dragging;
    press.active = false;
    if (dragging && (buttons & MOUSE_BUTTON_LEFT))
        put(OS_INPUT_MOUSE_UP, MOUSE_BUTTON_LEFT, press.x, press.y);
}
void touchWheel(int32_t x, int32_t y, int32_t delta) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!focused || !delta)
        return;
    put(OS_INPUT_MOUSE_MOVE, 0, x, y);
    put(OS_INPUT_MOUSE_WHEEL, delta, x, y);
}
// Soft keyboard: committed text as characters, editing keys as key presses.
void textInput(const std::u32string &text) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!focused)
        return;
    for (char32_t c : text)
        if (c >= 32 && c != 127)
            put(OS_INPUT_CHAR, int32_t(c), 0, 0);
}
void keyPress(int32_t which) {
    std::lock_guard<std::mutex> lock(inputMutex);
    if (!focused || which < 0 || which >= KEY_LAST)
        return;
    put(OS_INPUT_KEY_DOWN, which, 1, 0);
    put(OS_INPUT_KEY_UP, which, 1, 0);
}
void inputSurfaceSize(int32_t width, int32_t height) {
    if (width > 0 && height > 0) {
        std::lock_guard<std::mutex> lock(inputMutex);
        surfaceWidth = width;
        surfaceHeight = height;
        put(OS_INPUT_SIZE, width, height, 0);
    }
}
} // namespace wow::android
