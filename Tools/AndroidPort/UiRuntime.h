#pragma once
#include <string>
namespace wow::android {
// GL thread. Boots the original interface (FrameXML) once the Gx device exists and the
// client archives are mounted; returns false (with error) if it could not start.
bool uiBoot(int width, int height, const std::string &locale, const std::string &realm, std::string &error);
bool uiActive();
// An original EditBox (chat line, search box...) has keyboard focus: show the soft keyboard.
bool uiTextFocus();
// The original UI frame under the last pointer position (not WorldFrame/UIParent).
bool uiPointerOverFrame();
void uiResize(int width, int height);
// After the world is drawn, before GxScenePresent: UI layers (CSimpleTop) and events.
void uiPaint();
void uiSetZone(const std::string &zone);
// FrameXML files (newline separated) not loaded: they crashed the app in an earlier run.
void uiSetSkippedFiles(const std::string &list);
// Writable app directory for saved variables and macros (WTF/ under it).
void uiSetDataDirectory(const std::string &directory);
// Audio commands from the Lua game layer (__AndroidAudio) for the Java player, one per line:
// "music|volume|path;path…|silenceMin|silenceMax", "ambience|volume|path", "sound|volume|path",
// "stop|music" / "stop|ambience", "volume|master|music|sfx|ambience|enabled".
std::string uiTakeAudio();
// Any thread: Lua run on the GL thread at the next UI sync (Android back button: ToggleGameMenu).
void uiQueueScript(const std::string &script);
// One-line status (files loaded, errors, paints, last message) and the recent UI log.
std::string uiDiagnostics();
std::string uiLogText();
// Adds a line to the UI log (status dialog).
void uiLog(const std::string &line);
} // namespace wow::android
