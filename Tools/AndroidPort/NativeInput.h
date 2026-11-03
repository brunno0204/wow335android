#pragma once
#include <cstdint>
#include <string>

namespace wow::android {
// UI-thread producers, original OsInputGet consumer. No game/GLES calls here.
void inputFocus(bool focused);
void touchMovement(float right, float forward);
void touchCamera(bool held, float dx, float dy);
void touchJump(bool held);
void touchTap(int32_t x, int32_t y);
void touchPointer(int32_t x, int32_t y);
void touchDown(int32_t x, int32_t y);
void touchMove(int32_t x, int32_t y);
void touchUp(int32_t x, int32_t y);
// The finger became the movement stick: drop the pending press without clicking.
void touchCancel();
void touchWheel(int32_t x, int32_t y, int32_t delta);
void textInput(const std::u32string &text);
void keyPress(int32_t key);
void inputSurfaceSize(int32_t width, int32_t height);
// Analog stick (last touchMovement values, 0 when unfocused) for walk speed and backpedal.
void touchAnalog(float &right, float &forward);
// Camera finger held while free look is on: the drag turns only the camera.
void touchFreeLook(bool on);
bool touchFreeLookActive();
// GL thread, every frame: a few seconds after the last touch the pointer leaves the screen, so
// hover effects (tooltips opened by a tap) close like moving the mouse away.
void touchParkPointer();
} // namespace wow::android
