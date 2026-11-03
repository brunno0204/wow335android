#pragma once
class CSimpleFrame;
namespace wow::android {
// FrameXML factory for PlayerModel/DressUpModel/TabardModel: a CSimpleModel that draws the
// unit's loaded world model (WorldScene) into its rectangle.
CSimpleFrame *createUiModelFrame(CSimpleFrame *parent);
// __AndroidModelSetUnit/SetRotation/GetRotation/SetZoom for ui/70_models.lua.
void uiModelFramesRegister();
// Start of the GL frame, before the world: renders requested 3D portraits (2 per frame).
// Returns true when it drew (the caller clears the frame again).
bool uiRenderPortraits();
} // namespace wow::android
