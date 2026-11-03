#include <cstdint>
#include "ConsoleDevice.h"
#include "Engine/Source/ConsoleBase/IConsole.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxFormat.h"
#include "Engine/Source/Os/Gui.h"
#include "Engine/Source/Services/Texture.h"
#include "WoW/Source/Ui/GameUI.h"

// VERIFY: 6674b551 auto
// OFFSET: 0x00769FF0
int32_t CCGxRestart(const char* command, const char* arguments) {
    OnGxRestartClearWorld();
    ValidateFormatMonitor(s_requestedFormat);

    if (g_theGxDevicePtr->DeviceSetFormat(s_requestedFormat)) {
        s_lastGoodFormat = s_requestedFormat;
        UpdateGxCVars();
    } else {
        ConsoleWrite("unable to set requested display mode", DEFAULT_COLOR);
        s_requestedFormat = s_lastGoodFormat;

        if (!g_theGxDevicePtr->DeviceSetFormat(s_requestedFormat)) {
            ConsoleWrite("unable to set last good mode", DEFAULT_COLOR);
            s_requestedFormat = s_desktopFormat;

            if (!g_theGxDevicePtr->DeviceSetFormat(s_requestedFormat)) {
                ConsoleWrite("unable to set default format", DEFAULT_COLOR);
                s_requestedFormat = s_fallbackFormat;

                if (!g_theGxDevicePtr->DeviceSetFormat(s_requestedFormat)) {
                    goto done;
                }
            }

            s_lastGoodFormat = s_requestedFormat;
        }

        SetGxCVars(s_requestedFormat);
    }

done:
    OsGuiSetGxWindow(g_theGxDevicePtr->DeviceWindow());
    OsGuiSetWindowTitle(g_theGxDevicePtr->DeviceWindow(), s_windowTitle);
    TextureNotifyGxRestart();

    return 1;
}
