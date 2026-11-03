#pragma once
#include "PlayerState.h"
#include <memory>
namespace wow::android {
// GL thread: newest player snapshots (either may be null = unchanged).
void uiSetPlayerSnapshots(std::shared_ptr<const PlayerFieldsView> fields, std::shared_ptr<const PlayerDataView> data);
// GL thread, after FrameScript_Initialize: client database functions for ui/*.lua
// (__AndroidDbc, __AndroidDbcRow, __AndroidDbcCount).
void uiRegisterDataFunctions();
// GL thread, UI running: FactionTemplateReaction of the client (unit's template toward the
// viewer's): 1 hostile, 3 neutral, 4 friendly; -1 when a template is unknown.
int uiFactionReaction(uint32_t unitTemplate, uint32_t viewerTemplate);
} // namespace wow::android
