// Android platform layer for the original UI engine (Engine/Source/Frame, FrameXML, Gxu,
// Scrn, Services/Texture): OS services without an Android equivalent in this port (IME,
// clipboard, screenshots, movies, MPQ streaming/signatures), the sound engine (silent),
// and the 3D model hooks of CSimpleModel (no CM2Scene renderer: model frames stay empty).
#include "Engine/Source/Base/Status.h"
#include "Engine/Source/Event/Types.h"
#include "Engine/Source/Gx/Types.h"
#include "Engine/Source/Gxu/GxuScene.h"
#include "Engine/Source/Images/ImageWriter.h"
#include "Engine/Source/Model2/M2Cache.h"
#include "Engine/Source/Model2/M2Model.h"
#include "Engine/Source/Model2/M2Scene.h"
#include "Engine/Source/Os/OsClipboard.h"
#include "Engine/Source/Os/OsIME.h"
#include "Engine/Source/Os/Path.h"
#include "Engine/Source/Os/System.h"
#include "Engine/Source/Os/Time.h"
#include "Engine/Source/Services/Camera.h"
#include "Engine/Source/Services/SysMessage.h"
#include "Engine/Source/SoundEngine/SoundEngine.h"
#include "Engine/Source/Storm/H/storm.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/Frame/CSimpleRender.h"
#include "ThirdParty/DivxDecoder/DivxDecoder.h"
#include "WoW/Source/Glue/CSimpleModelFFX.h"
#include <android/log.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>

// ---- Fonts: FRIZQT__.TTF and friends are read from the MPQs (Fonts\).
void OsBuildFontFilePath(const char *fontName, char *path, uint32_t size) {
    if (!path || !size)
        return;
    std::snprintf(path, size, "Fonts\\%s", fontName ? fontName : "");
}

// ---- Clipboard / IME: not available to the GL thread on Android.
static char *s_clipboard = nullptr;
char *OsClipboardGetString() {
    if (!s_clipboard)
        return nullptr;
    char *copy = static_cast<char *>(std::malloc(std::strlen(s_clipboard) + 1));
    std::strcpy(copy, s_clipboard);
    return copy;
}
int32_t OsClipboardPutString(const char *string) {
    std::free(s_clipboard);
    s_clipboard = string ? strdup(string) : nullptr;
    return 1;
}
void OsClipboardFreeString(char *string) { std::free(string); }
OS_IME_LANGUAGEMODE OsIMEGetLanguageMode() { return OS_IME_MODE_ROMAN; }
void OsIMESetLanguageMode(int32_t) {}
void OsIMEEnable(int32_t) {}
void OsIMESync() {}
int32_t OsIMEGetCompositionString(char *buffer, uint32_t size) {
    if (buffer && size)
        buffer[0] = 0;
    return 0;
}
int32_t OsIMEGetCompositionResult(char *buffer, uint32_t size) { return OsIMEGetCompositionString(buffer, size); }
int32_t OsIMEGetClauseInfo(uint32_t &start, uint32_t &end, uint32_t &cursor) {
    start = end = cursor = 0;
    return 0;
}
int32_t OsIMEGetCandidates(uint32_t, uint32_t &pageStart, uint32_t &pageSize, uint32_t &count, uint32_t &selection,
                           TSGrowableArray<OsIMECandidate> &) {
    pageStart = pageSize = count = selection = 0;
    return 0;
}

// ---- System.
uint32_t OsGetBackgroundSleepMs() { return 0; }
int32_t OsSleepInBackground() { return 0; }
uint64_t OsGetPhysicalMemory() {
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? uint64_t(pages) * uint64_t(size) : 2ull << 30;
}
void OsGetTimeStamp(char *buffer, uint32_t size) {
    if (!buffer || !size)
        return;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::strftime(buffer, size, "%m%d%y_%H%M%S", &local);
}
int32_t SysMsgAdd(CStatus *) { return 1; }
void SErrDisplayAppFatal(const char *format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format ? format : "", args);
    va_end(args);
    __android_log_print(ANDROID_LOG_ERROR, "WowPort", "UI fatal: %s", text);
}
int32_t STORMAPI SErrGetErrorStr(uint32_t code, char *buffer, uint32_t chars) {
    if (buffer && chars)
        std::snprintf(buffer, chars, "error %u", code);
    return 1;
}

// ---- MPQ streaming (full local installs only) and interface signatures.
int32_t SFile::IsStreamingMode() { return 0; }
bool STORMAPI SFile::FileGetIsLocalAmount(const char *, int64_t &local, int64_t &total) {
    local = total = 1;
    return true;
}
int32_t STORMAPI SFile::FileIsLocal(SFileImpl *, int32_t) { return 1; }
int32_t STORMAPI SFile::MakeFileLocal(SFileImpl *, int32_t, bool) { return 1; }
void STORMAPI SFile::SetFilePriority(SFileImpl *, int32_t, int32_t) {}
struct SSignatureData {};
void SSignatureVerifyStream_Begin(SSignatureData **token, uint32_t, uint32_t) {
    if (token)
        *token = nullptr;
}
void SSignatureVerifyStream_ProvideData(SSignatureData *, const uint8_t *, uint32_t) {}
uint32_t SSignatureVerifyStream_GetSignatureLength(SSignatureData *) { return 0; }
int32_t SSignatureVerifyStream_Finish(SSignatureData *, const uint8_t *, const uint8_t *) { return 0; }

// ---- Screenshots and movies.
int32_t ImageWriterInitialize(uint32_t, void *(*)(size_t), void (*)(void *), int32_t) { return 0; }
int32_t ImageWriterShutdown() { return 1; }
int32_t ImageWriterEmbedData(const void *, uint32_t, uint32_t, uint32_t, const uint8_t *, uint32_t) { return 0; }
int32_t ImageWriterWriteJpeg(const char *, const void *, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
int32_t InitializeDivxDecoder(int32_t, int32_t, int32_t) { return 0; }
int32_t SetOutputFormat(int32_t, int32_t, int32_t, int32_t) { return 0; }
int32_t DivxDecode(int32_t, DivxDecodeFrame *, int32_t) { return 0; }
int32_t UnInitializeDivxDecoder(int32_t) { return 0; }

// ---- Game hooks referenced by engine frames.
MOUSEBUTTON StringToMouseButton(const char *string) {
    if (!string)
        return MOUSE_BUTTON_NONE;
    if (!std::strcmp(string, "LeftButton") || !std::strcmp(string, "BUTTON1"))
        return MOUSE_BUTTON_LEFT;
    if (!std::strcmp(string, "RightButton") || !std::strcmp(string, "BUTTON2"))
        return MOUSE_BUTTON_RIGHT;
    if (!std::strcmp(string, "MiddleButton") || !std::strcmp(string, "BUTTON3"))
        return MOUSE_BUTTON_MIDDLE;
    return MOUSE_BUTTON_NONE;
}
int32_t GetDeclinedWord(const char *word, int32_t, char *buffer, uint32_t size) {
    if (buffer && size)
        std::snprintf(buffer, size, "%s", word ? word : "");
    return 1;
}
void EmblemTextureUpdate(EGxTexCommand, uint32_t, uint32_t, uint32_t, uint32_t, void *, uint32_t &stride,
                         const void *&texels) {
    static const uint32_t clear[64 * 64] = {};
    stride = 64 * 4;
    texels = clear;
}
void CSimpleModelFFX::CreateScriptMetaTable() {}

// ---- 3D model frames: no CM2Scene renderer in this port yet.
CM2Scene *M2CreateScene() {
    static_assert(sizeof(CM2Scene) > 0);
    return static_cast<CM2Scene *>(std::calloc(1, sizeof(CM2Scene)));
}
void CM2Scene::AdvanceTime(uint32_t) {}
int32_t CM2Scene::Animate(const C3Vector &) { return 1; }
CM2Model *CM2Scene::CreateModel(const char *, uint32_t) { return nullptr; }
void CM2Scene::Draw(M2PASS) {}
void CM2Scene::Release() {}
CAaBox CM2Model::GetBoundingBox() { return CAaBox{}; }
CCamera *CM2Model::GetCameraById(uint32_t) { return nullptr; }
void *CM2Model::GetCameraByIndex(uint32_t) { return nullptr; }
int32_t CM2Model::IsDrawable(int32_t, int32_t) { return 0; }
uint32_t CM2Model::IsLoaded(int32_t, int32_t) { return 0; }
int32_t CM2Model::Release() { return 0; }
void CM2Model::ReplaceTexture(uint32_t, CTexture *) {}
void CM2Model::SetAnimating(int32_t) {}
void CM2Model::SetBoneSequence(uint32_t, uint32_t, uint32_t, uint32_t, float, int32_t, int32_t) {}
void CM2Model::SetLoadedCallback(void (*)(CM2Model *, void *), void *) {}
void CM2Model::SetSequenceCallback(void *, uint64_t) {}
void CM2Model::SetWorldTransform(const C3Vector &, float, float) {}
void CM2Model::WaitForLoad(const char *) {}

// ---- Sound engine: silent (button/UI sounds are not played).
void GxuSceneObject::Unk00() {}
void GxuSceneObject::Release() {}
bool GxuSceneObject::IsAlive() { return false; }
void GxuSceneObject::Update(float) {}
void GxuSceneObject::Render() {}
int32_t SE2::HeartBeat(const void *, void *) { return 1; }
SESound::SESound() { std::memset(static_cast<void *>(this), 0, sizeof(*this)); }
SESound::~SESound() {}
uint32_t SESound::GetPosition() { return 0; }
bool SESound::IsPlaying() { return false; }
void SESound::Play() {}
void SESound::PlaySoundFromMemory(const char *, uint32_t, int32_t) {}
void SESound::SetChannelGroup(const char *, bool) {}
void SESound::SetFinishedCallback2(void (*)(SESound *, void *), void *) {}
void SESound::SetPosition(uint32_t) {}
void SESound::SetVolume(float) {}
void SESound::Stop(int32_t, float) {}
