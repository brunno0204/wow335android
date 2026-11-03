#include "TerrainModelPlacement.h"
#include "WorldScene.h"
#include "UiRuntime.h"
#include "CrashReport.h"
#include "UiModelFrames.h"
#include "AssetLock.h"
#include "GlueResources.h"
#include "ClientData.h"
#include "CGxDeviceGles.h"
#include "NativeInput.h"
#include "WorldSky.h"
#include "NativeEvents.h"
#include "M2ModelPreview.h"
#include "M2PreviewTextures.h"
#include "TerrainPreview.h"
#include "TerrainPreviewTextures.h"
#include "TerrainSurface.h"
#include "WorldAssets.h"
#include "WorldLink.h"
#include "GlueBridgeInternal.h"
#include "TerrainObjects.h"
#include "WmoModel.h"
#include "TerrainGeometry.h"
#include "M2File.h"
#include <map>
#include <optional>
#include <mutex>
#include <cctype>
#include <set>
#include <atomic>
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Event/Event.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/Os/Input.h"
#include "Engine/Source/Gx/Gx.h"
#include <android/log.h>
#include <jni.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <stdexcept>

static_assert(sizeof(void *) == 8, "ARM64 build required");
namespace {
CGxDeviceGles *device = nullptr;
CGxTex *texture = nullptr;
CShaderEffect worldMaterial;
CM2Lighting worldLighting;
void textureSource(EGxTexCommand command, uint32_t width, uint32_t height, uint32_t face, uint32_t level,
                   void *, uint32_t &stride, const void *&texels) {
    static const auto bytes = [] {
        std::array<uint8_t, 8 * 8 * 4> data{};
        for (size_t i = 0; i < data.size(); i += 4) {
            data[i] = 59;
            data[i + 1] = 173;
            data[i + 2] = 242;
            data[i + 3] = 255;
        }
        return data;
    }();
    if (command == GxTex_Latch) {
        if (width != 8 || height != 8 || face || level)
            throw std::runtime_error("Invalid default texture request");
        stride = 32;
        texels = bytes.data();
    }
}
// Asset mounts are process-global. Worker-only scanning/loading serialize here;
// the GL thread consumes only owned decoded data and never takes this IO lock.
std::mutex assetIoMutex, pendingPreviewMutex;
std::shared_ptr<wow::android::LocalClientData> glueResources;
std::atomic<bool> resetPreviewClock{true};
std::atomic<bool> resetProbeInput{false};
wow::android::TerrainProbeInput probeInput;
// Glue scene (GL thread only).
std::unique_ptr<wow::android::GlueSceneData> glueScene;
wow::android::M2PreviewTextures *glueBackgroundTextures = nullptr, *glueCharacterTextures = nullptr;
std::vector<std::unique_ptr<wow::android::M2PreviewTextures>> glueItemTextures;
uint64_t glueStart = 0;
std::atomic<bool> clearWorldScene{false};
uint64_t frameMilliseconds = 0;
bool graphicsReady = false;
float aspect = 1.0f;
// Original FrameXML interface: wanted by the user setting, booted on the GL thread once the
// world is shown (the client archives are mounted by then).
std::atomic<bool> originalUiWanted{false}, originalUiRunning{false};
std::atomic<int> originalUiStatus{0}; // 0 off, 1 running, 2 failed
std::atomic<bool> uiPointerCaptured{false}, uiTextWanted{false};
std::mutex originalUiErrorMutex;
std::string originalUiError;
void setOriginalUiError(const std::string &text) {
    std::lock_guard<std::mutex> lock(originalUiErrorMutex);
    originalUiError = text;
}
uint64_t worldShownSince = 0;
// World taps are resolved on the GL thread after the UI saw the same pointer position:
// a tap on an original UI frame does not also select a unit behind it.
std::mutex pendingTapMutex;
std::array<float, 4> pendingTap{};
bool tapPending = false;
void resolveTap() {
    std::array<float, 4> tap;
    {
        std::lock_guard<std::mutex> lock(pendingTapMutex);
        if (!tapPending)
            return;
        tapPending = false;
        tap = pendingTap;
    }
    if (wow::android::uiActive() && wow::android::uiPointerOverFrame())
        return;
    wow::android::worldSceneTap(tap[0], tap[1], tap[2], tap[3]);
}
int surfaceWidth = 1, surfaceHeight = 1;
void paintOriginalUi() {
    if (!originalUiWanted.load())
        return;
    if (!wow::android::uiActive()) {
        if (originalUiStatus.load() == 2 || frameMilliseconds - worldShownSince < 1500)
            return;
        std::string locale, realm, error;
        {
            std::lock_guard<std::mutex> link(wow::android::worldLinkMutex());
            locale = wow::android::worldLink().locale;
            realm = wow::android::worldLink().realmName;
        }
        wow::android::crashBreadcrumb("UI: boot");
        if (!wow::android::uiBoot(surfaceWidth, surfaceHeight, locale, realm, error)) {
            originalUiStatus.store(2);
            setOriginalUiError(error);
            __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Original UI: %s", error.c_str());
            return;
        }
        originalUiStatus.store(1);
    }
    wow::android::uiPaint();
    uiPointerCaptured.store(wow::android::uiPointerOverFrame());
    uiTextWanted.store(wow::android::uiTextFocus());
    wow::android::crashBreadcrumb("World frame");
    originalUiRunning.store(true);
}
int32_t paintWorld(const void *, void *);
int32_t probeKeyDown(const void *payload,void *) {
    probeInput.key(static_cast<const EVENT_DATA_KEY *>(payload)->m_key,true);return 1;
}
int32_t probeKeyUp(const void *payload,void *) {
    probeInput.key(static_cast<const EVENT_DATA_KEY *>(payload)->m_key,false);return 1;
}
int32_t probeFocus(const void *payload,void *) {
    if(!static_cast<const EVENT_DATA_FOCUS *>(payload)->m_focus)probeInput.clear();
    return 1;
}
int32_t cameraModeOnMouseDown(const void *payload, void *) {
    const auto &event = *static_cast<const EVENT_DATA_MOUSE *>(payload);
    if (event.m_button == MOUSE_BUTTON_RIGHT)
        EventSetMouseMode(MOUSE_MODE_RELATIVE, MOUSE_BUTTON_RIGHT);
    return 1;
}
int32_t orbitPreview(const void *payload, void *) {
    const auto &event = *static_cast<const EVENT_DATA_MOUSE *>(payload);
    if (event.m_mode == MOUSE_MODE_RELATIVE && (event.m_buttonState & MOUSE_BUTTON_RIGHT)) {
        wow::android::worldSceneOrbit(event.m_x, event.m_y);
    }
    return 1;
}
int32_t initializeEventContext(const void *, void *) {
    EventRegister(EVENT_ID_MOUSEDOWN, cameraModeOnMouseDown);
    EventRegister(EVENT_ID_PAINT, paintWorld);
    EventRegister(EVENT_ID_MOUSEMOVE_RELATIVE, orbitPreview);
    EventRegister(EVENT_ID_KEYDOWN,probeKeyDown);
    EventRegister(EVENT_ID_KEYUP,probeKeyUp);
    EventRegister(EVENT_ID_FOCUS,probeFocus);
    return 1;
}
} // namespace

namespace {
std::string javaString(JNIEnv *env, jstring value, size_t limit = 4096) {
    if (!value)
        throw std::invalid_argument("Missing path");
    if (size_t(env->GetStringUTFLength(value)) > limit)
        throw std::invalid_argument("Path is too long");
    const char *bytes = env->GetStringUTFChars(value, nullptr);
    if (!bytes)
        throw std::runtime_error("Cannot access path");
    try {
        std::string result(bytes);
        env->ReleaseStringUTFChars(value, bytes);
        return result;
    } catch (...) {
        env->ReleaseStringUTFChars(value, bytes);
        throw;
    }
}
} // namespace
extern "C" JNIEXPORT jstring JNICALL
Java_org_wowdecomp_android_MainActivity_openGlueResources(JNIEnv *env, jclass, jstring root) {
    std::lock_guard<std::mutex> lock(assetIoMutex);
    try {
        glueResources.reset();
        glueResources = wow::android::sharedClientData(javaString(env, root));
        if (!glueResources->report().ready) {
            auto report = glueResources->report().summary();
            glueResources.reset();
            return env->NewStringUTF(report.c_str());
        }
        return nullptr;
    } catch (const std::exception &) {
        glueResources.reset();
        return env->NewStringUTF("Não foi possível abrir os MPQs da pasta Data.");
    }
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_wowdecomp_android_MainActivity_readGlueResource(JNIEnv *env, jclass, jstring path) {
    std::lock_guard<std::mutex> lock(assetIoMutex);
    try {
        if (!glueResources)
            throw std::runtime_error("Glue resource reader is not open");
        auto bytes = wow::android::readMountedGlueResource(javaString(env, path, 512));
        if (!bytes)
            return nullptr;
        auto result = env->NewByteArray(jsize(bytes->size()));
        if (result && !bytes->empty())
            env->SetByteArrayRegion(result, 0, jsize(bytes->size()),
                                    reinterpret_cast<const jbyte *>(bytes->data()));
        return result;
    } catch (const std::exception &error) {
        env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"), error.what());
        return nullptr;
    }
}
extern "C" JNIEXPORT void JNICALL
Java_org_wowdecomp_android_MainActivity_closeGlueResources(JNIEnv *, jclass) {
    std::lock_guard<std::mutex> lock(assetIoMutex);
    glueResources.reset();
}
extern "C" JNIEXPORT jboolean JNICALL
Java_org_wowdecomp_android_MainActivity_initializeGraphics(JNIEnv *, jclass) {
    wow::android::crashReportThread();
    graphicsReady = false;
    resetPreviewClock.store(true);
    try {
        // The original Gx device owns logical resources across EGL replacements.
        if (device)
            device->RestoreCurrentContext();
        else {
            CGxFormat format(true, C2iVector{0, 0}, CGxFormat::Fmt_Argb8888, CGxFormat::Fmt_Ds160,
                             0, 1, true, false, false, true, 0);
            auto *created = CreateGxDevice(GxDefaultApi(), nullptr, format);
            device = dynamic_cast<CGxDeviceGles *>(created);
            if (!device)
                throw std::runtime_error("Native original Gx device creation failed");
        }
        if (!texture && !GxTexCreate(8, 8, GxTex_Argb8888, CGxTexFlags(GxTex_Linear), nullptr,
                                     textureSource, texture))
            throw std::runtime_error("Original Gx texture creation failed");
        CShaderEffect::InitShaderSystem(0, 0);
        worldMaterial.InitEffect(nullptr, nullptr);
        const EGxTexOp operations[1]{GxTexOp_Mod};
        worldMaterial.InitFixedFuncPass(operations, operations, 1);
        worldMaterial.SetCurrent();
        worldLighting.Initialize(nullptr, CAaSphere{});
        worldLighting.AddAmbient(C3Vector(1, 1, 1));
        CShaderEffect::SetLocalLighting(&worldLighting, 1, nullptr);
        CShaderEffect::SetFogEnabled(0);
        GxRsSet(GxRs_Culling, 0);
        GxRsSet(GxRs_DepthTest, 0);
        GxRsSet(GxRs_DepthWrite, 0);
        GxRsSet(GxRs_Texture0, texture);
        GxXformSetProjection(C44Matrix(1.0f));
        GxXformSetView(C44Matrix(1.0f));
        if (!wow::android::eventsInitialized()) {
            EventInitialize(1, 0);
            EventCreateContextEx(1, initializeEventContext, nullptr, 1, 0);
        } else {
            // GLSurfaceView has quiesced the previous GL thread before replacement.
            wow::android::rebindEventsToCurrentThread();
        }
        graphicsReady = true;
        return JNI_TRUE;
    } catch (const std::exception &error) {
        __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Graphics initialization: %s", error.what());
        return JNI_FALSE;
    }
}

extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_resizeGraphics(JNIEnv *, jclass,
                                                                                         jint width,
                                                                                         jint height) {
    if (!graphicsReady || width <= 0 || height <= 0)
        return;
    try {
        auto format = device->m_format;
        format.m_size = {width, height};
        if (!device->DeviceSetFormat(format))
            throw std::runtime_error("Original Gx resize failed");
        aspect = float(width) / float(height);
        surfaceWidth = width;
        surfaceHeight = height;
        wow::android::inputSurfaceSize(width, height);
        wow::android::uiResize(width, height);
    } catch (const std::exception &error) {
        graphicsReady = false;
        __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Graphics resize: %s", error.what());
    }
}

namespace {
// Row-vector matrices as in TerrainPreview: camera looks down -Z, Z-up world.
C44Matrix lookAt(const wow::android::Vec3 &eye, const wow::android::Vec3 &target) {
    double f[3]{target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    double n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (n < 1e-9) { f[0] = 1; f[1] = f[2] = 0; n = 1; }
    for (double &v : f) v /= n;
    double r[3]{f[1], -f[0], 0}; // f x up(0,0,1)
    double rn = std::sqrt(r[0] * r[0] + r[1] * r[1]);
    if (rn < 1e-9) { r[0] = 1; r[1] = 0; rn = 1; }
    r[0] /= rn; r[1] /= rn;
    const double u[3]{r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    C44Matrix m(1);
    m.a0 = float(r[0]); m.b0 = float(r[1]); m.c0 = float(r[2]);
    m.a1 = float(u[0]); m.b1 = float(u[1]); m.c1 = float(u[2]);
    m.a2 = float(-f[0]); m.b2 = float(-f[1]); m.c2 = float(-f[2]);
    m.d0 = float(-(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2]));
    m.d1 = float(-(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]));
    m.d2 = float(f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    return m;
}
C44Matrix perspective(double fovy, double aspectRatio, double nearPlane, double farPlane) {
    const double f = 1 / std::tan(fovy / 2);
    C44Matrix m(0);
    m.a0 = float(f / aspectRatio);
    m.b1 = float(f);
    m.c2 = float(-(farPlane + nearPlane) / (farPlane - nearPlane));
    m.c3 = -1;
    m.d2 = float(-2 * farPlane * nearPlane / (farPlane - nearPlane));
    return m;
}
// ModelFFX scene: camera 0 of the background (CSimpleModel), character on attachment 0.
void drawGlueScene() {
    auto &scene = *glueScene;
    const uint32_t t = uint32_t(frameMilliseconds - std::min(frameMilliseconds, glueStart));
    scene.background->frame(frameMilliseconds);
    if (scene.character) scene.character->frame(frameMilliseconds);
    worldMaterial.SetCurrent();
    if (!scene.info.hasCamera) {
        scene.background->draw(aspect, glueBackgroundTextures);
        return;
    }
    const uint32_t time = scene.info.duration ? t % scene.info.duration : t;
    const auto eye = scene.info.position.at(time, scene.info.positionBase);
    const auto target = scene.info.target.at(time, scene.info.targetBase);
    // GxuXformCreateProjection_SG: stored FOV is diagonal.
    const double fovy = scene.info.fov / std::sqrt(1.0 + double(aspect) * aspect);
    const auto view = lookAt(eye, target);
    const auto projection = perspective(fovy, aspect, scene.info.nearClip, scene.info.farClip);
    scene.background->drawPlaced(C44Matrix(1), view, projection, glueBackgroundTextures);
    if (!scene.particles && scene.background->effects() && !scene.background->effects()->particles.empty())
        scene.particles = std::make_unique<wow::android::M2ParticleRuntime>(scene.background->effects());
    if (scene.particles) {
        static uint64_t lastParticleFrame = 0;
        const float seconds = lastParticleFrame && frameMilliseconds > lastParticleFrame
                                  ? std::min(0.1f, float(frameMilliseconds - lastParticleFrame) / 1000.f)
                                  : 0.f;
        lastParticleFrame = frameMilliseconds;
        const auto &playback = scene.background->model().playback();
        scene.particles->update(seconds, scene.background->model().bones(), playback.position(), playback.globalTime());
        scene.particles->draw(view, projection);
    }
    if (scene.character) {
        const auto anchor = scene.info.attachment(0).value_or(wow::android::Vec3{0, 0, 0});
        const float facing = wow::android::glueFacing().load();
        C44Matrix world(1);
        world.a0 = std::cos(facing); world.a1 = std::sin(facing);
        world.b0 = -std::sin(facing); world.b1 = std::cos(facing);
        world.d0 = float(anchor[0]); world.d1 = float(anchor[1]); world.d2 = float(anchor[2]);
        worldMaterial.SetCurrent();
        scene.character->drawPlaced(world, view, projection, glueCharacterTextures);
        for (size_t i = 0; i < scene.items.size() && i < glueItemTextures.size(); ++i)
            if (auto attached = scene.character->model().attachment(scene.items[i].second, world)) {
                worldMaterial.SetCurrent();
                scene.items[i].first->drawPlaced(*attached, view, projection, glueItemTextures[i].get());
            }
    }
}
int32_t paintWorld(const void *, void *) {
    const bool jump = probeInput.consumeJump();
    if (!graphicsReady)
        return 1;
    try {
        const bool world = wow::android::worldSceneActive();
        // Sky blue in the world (CImVector is b, g, r, a).
        CImVector skyColor{230, 175, 120, 255};
        if (world) {
            const uint32_t c = wow::android::worldSkyClearColor();
            skyColor = CImVector{uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16), 255};
        }
        GxSceneClear(GxBufferBit_Color | GxBufferBit_Depth, world ? skyColor : CImVector{23, 14, 9, 255});
        if (world && wow::android::uiActive() && originalUiRunning.load()) {
            try {
                if (wow::android::uiRenderPortraits())
                    GxSceneClear(GxBufferBit_Color | GxBufferBit_Depth, skyColor);
            } catch (const std::exception &error) {
                __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Portraits: %s", error.what());
            }
        }
        if (world) {
            if (!worldShownSince)
                worldShownSince = frameMilliseconds ? frameMilliseconds : 1;
            wow::android::WorldInput input;
            input.right = probeInput.right();
            input.forward = probeInput.forward();
            float stickRight = 0, stickForward = 0;
            wow::android::touchAnalog(stickRight, stickForward);
            if (stickRight != 0 || stickForward != 0) { // the touch stick is analog (walk, backpedal)
                input.right = stickRight;
                input.forward = stickForward;
            }
            input.freeLook = wow::android::touchFreeLookActive();
            wow::android::touchParkPointer();
            input.jump = jump;
            wow::android::worldScenePaint(frameMilliseconds, aspect, input, worldMaterial);
            resolveTap();
            try {
                paintOriginalUi();
            } catch (const std::exception &error) {
                originalUiStatus.store(2);
                setOriginalUiError(std::string("ao desenhar: ") + error.what());
                __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Original UI frame: %s", error.what());
            }
        } else {
            worldShownSince = 0;
            originalUiRunning.store(false);
            if (glueScene && glueScene->background)
                drawGlueScene();
        }
        GxScenePresent();
    } catch (const std::exception &error) {
        graphicsReady = false;
        __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Graphics frame: %s", error.what());
    }
    return 1;
}
} // namespace

extern "C" JNIEXPORT void JNICALL
Java_org_wowdecomp_android_MainActivity_drawFrame(JNIEnv *, jclass, jlong milliseconds) {
    if (!graphicsReady)
        return;
    if (milliseconds < 0)
        return;
    frameMilliseconds = uint64_t(milliseconds);
    try {
        wow::android::worldSceneAdopt();
        std::unique_ptr<wow::android::GlueSceneData> glueIncoming;
        {
            std::lock_guard<std::mutex> lock(wow::android::glueSceneMutex());
            glueIncoming = std::move(wow::android::pendingGlueScene());
        }
        if (glueIncoming) {
            std::unique_ptr<wow::android::M2PreviewTextures> background, character;
            if (glueIncoming->background)
                background = std::make_unique<wow::android::M2PreviewTextures>(
                    glueIncoming->background->model().previewMaterials());
            if (glueIncoming->character)
                character = std::make_unique<wow::android::M2PreviewTextures>(
                    glueIncoming->character->model().previewMaterials());
            std::unique_ptr<wow::android::M2PreviewTextures> retiredBackground(glueBackgroundTextures),
                retiredCharacter(glueCharacterTextures);
            glueBackgroundTextures = background.release();
            glueCharacterTextures = character.release();
            glueItemTextures.clear();
            for (const auto &item : glueIncoming->items)
                glueItemTextures.push_back(
                    std::make_unique<wow::android::M2PreviewTextures>(item.first->model().previewMaterials()));
            if (glueIncoming->resetFacing) wow::android::glueFacing().store(0);
            glueScene = glueIncoming->background ? std::move(glueIncoming) : nullptr;
            glueStart = frameMilliseconds;
            if (glueScene) {
                glueScene->background->resetFrameClock();
                if (glueScene->character) glueScene->character->resetFrameClock();
            }
        }
        if (clearWorldScene.exchange(false))
            wow::android::worldSceneClear();
        resetPreviewClock.store(false);
        if(resetProbeInput.exchange(false))probeInput.clear();
        if (!wow::android::pumpEvents())
            graphicsReady = false;
    } catch (const std::exception &error) {
        graphicsReady = false;
        __android_log_print(ANDROID_LOG_ERROR, "WowPort", "Event frame: %s", error.what());
    }
}

extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_initializeInput(JNIEnv *,
                                                                                          jclass) {
    OsInputInitialize();
}
extern "C" JNIEXPORT void JNICALL
Java_org_wowdecomp_android_MainActivity_inputFocus(JNIEnv *, jclass, jboolean focused) {
    wow::android::inputFocus(focused == JNI_TRUE);
    if (focused != JNI_TRUE) {
        resetPreviewClock.store(true);
        resetProbeInput.store(true);
    }
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchMovement(
    JNIEnv *, jclass, jfloat right, jfloat forward) {
    wow::android::touchMovement(right, forward);
}
// Android back button in the world: the original Escape behaviour (close windows, game menu).
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_uiBackPressed(JNIEnv *, jclass) {
    wow::android::uiQueueScript("if __AndroidEscape then __AndroidEscape() end");
}
extern "C" JNIEXPORT jstring JNICALL Java_org_wowdecomp_android_MainActivity_pollAudio(JNIEnv *env, jclass) {
    const auto text = wow::android::uiTakeAudio();
    return env->NewStringUTF(text.c_str());
}
// A file from the mounted archives (sounds for the Java player), or null.
extern "C" JNIEXPORT jbyteArray JNICALL Java_org_wowdecomp_android_MainActivity_readArchiveFile(JNIEnv *env, jclass,
                                                                                               jstring path) {
    try {
        const auto name = javaString(env, path, 1024);
        std::vector<uint8_t> bytes;
        {
            std::lock_guard<std::mutex> lock(assetIoMutex);
            bytes = wow::android::m2::readFile(name, 128 * 1024 * 1024);
        }
        jbyteArray array = env->NewByteArray(jsize(bytes.size()));
        if (array && !bytes.empty())
            env->SetByteArrayRegion(array, 0, jsize(bytes.size()), reinterpret_cast<const jbyte *>(bytes.data()));
        return array;
    } catch (const std::exception &) {
        return nullptr;
    }
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchFreeLook(JNIEnv *, jclass,
                                                                                      jboolean on) {
    wow::android::touchFreeLook(on == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchCamera(
    JNIEnv *, jclass, jboolean held, jfloat dx, jfloat dy) {
    wow::android::touchCamera(held == JNI_TRUE, dx, dy);
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchJump(JNIEnv *,
                                                                                    jclass,
                                                                                    jboolean held) {
    wow::android::touchJump(held == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchTap(JNIEnv *, jclass, jint x,
                                                                                   jint y) {
    wow::android::touchTap(x, y);
}

extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchPointer(JNIEnv *, jclass,
                                                                                       jint x, jint y) {
    wow::android::touchPointer(x, y);
}

extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchDown(JNIEnv *, jclass, jint x,
                                                                                    jint y) {
    wow::android::touchDown(x, y);
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchMove(JNIEnv *, jclass, jint x,
                                                                                    jint y) {
    wow::android::touchMove(x, y);
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchCancel(JNIEnv *, jclass) {
    wow::android::touchCancel();
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchUp(JNIEnv *, jclass, jint x,
                                                                                  jint y) {
    wow::android::touchUp(x, y);
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchWheel(JNIEnv *, jclass, jint x,
                                                                                     jint y, jint delta) {
    wow::android::touchWheel(x, y, delta);
}
// The finger is over an original UI frame (last frame's mouse focus): no camera, no target.
extern "C" JNIEXPORT jboolean JNICALL Java_org_wowdecomp_android_MainActivity_uiPointerCaptured(JNIEnv *,
                                                                                               jclass) {
    return originalUiRunning.load() && uiPointerCaptured.load() ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT jboolean JNICALL Java_org_wowdecomp_android_MainActivity_uiTextWanted(JNIEnv *, jclass) {
    return originalUiRunning.load() && uiTextWanted.load() ? JNI_TRUE : JNI_FALSE;
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_textInput(JNIEnv *env, jclass,
                                                                                    jstring text) {
    if (!text)
        return;
    const jsize length = env->GetStringLength(text);
    const jchar *chars = env->GetStringChars(text, nullptr);
    if (!chars)
        return;
    std::u32string decoded;
    for (jsize i = 0; i < length; ++i) {
        char32_t c = chars[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < length && chars[i + 1] >= 0xDC00 && chars[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (chars[i + 1] - 0xDC00);
            ++i;
        }
        decoded.push_back(c);
    }
    env->ReleaseStringChars(text, chars);
    wow::android::textInput(decoded);
}
// 0 Enter, 1 Backspace, 2 Escape, 3 Tab, 4 Left, 5 Right, 6 Up, 7 Down.
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_keyInput(JNIEnv *, jclass, jint which) {
    static const KEY keys[] = {KEY_ENTER, KEY_BACKSPACE, KEY_ESCAPE, KEY_TAB, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN};
    if (which >= 0 && which < jint(sizeof(keys) / sizeof(keys[0])))
        wow::android::keyPress(keys[which]);
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_clearWorldScene(JNIEnv *,
                                                                                        jclass) {
    clearWorldScene.store(true);
}

namespace wow::android {
std::mutex &assetIoLock() { return assetIoMutex; }
} // namespace wow::android
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_touchZoom(JNIEnv *, jclass,
                                                                                    jfloat factor) {
    if (std::isfinite(factor) && factor > 0)
        wow::android::worldSceneZoom(factor);
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_worldTap(JNIEnv *, jclass, jfloat x,
                                                                                   jfloat y, jfloat width,
                                                                                   jfloat height) {
    std::lock_guard<std::mutex> lock(pendingTapMutex);
    pendingTap = {x, y, width, height};
    tapPending = true;
}
extern "C" JNIEXPORT jstring JNICALL Java_org_wowdecomp_android_MainActivity_worldOverlay(JNIEnv *env, jclass) {
    const auto text = wow::android::worldSceneOverlay();
    if (text.empty())
        return nullptr;
    // Names may hold any UTF-8; build through byte[] to avoid modified-UTF-8 issues.
    auto bytes = env->NewByteArray(jsize(text.size()));
    if (!bytes)
        return nullptr;
    env->SetByteArrayRegion(bytes, 0, jsize(text.size()), reinterpret_cast<const jbyte *>(text.data()));
    auto type = env->FindClass("java/lang/String");
    auto constructor = env->GetMethodID(type, "<init>", "([BLjava/lang/String;)V");
    auto encoding = env->NewStringUTF("UTF-8");
    auto result = static_cast<jstring>(env->NewObject(type, constructor, bytes, encoding));
    env->DeleteLocalRef(bytes);
    env->DeleteLocalRef(encoding);
    env->DeleteLocalRef(type);
    return result;
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_installCrashReport(JNIEnv *env, jclass,
                                                                                            jstring path) {
    const char *chars = env->GetStringUTFChars(path, nullptr);
    if (chars) {
        wow::android::installCrashReport(chars);
        std::string directory(chars);
        const auto slash = directory.rfind('/');
        wow::android::uiSetDataDirectory(slash == std::string::npos ? std::string(".") : directory.substr(0, slash));
        env->ReleaseStringUTFChars(path, chars);
    }
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_setUiSkippedFiles(JNIEnv *env, jclass,
                                                                                           jstring list) {
    const char *chars = env->GetStringUTFChars(list, nullptr);
    if (chars) {
        wow::android::uiSetSkippedFiles(chars);
        env->ReleaseStringUTFChars(list, chars);
    }
}
extern "C" JNIEXPORT void JNICALL Java_org_wowdecomp_android_MainActivity_setOriginalUi(JNIEnv *, jclass,
                                                                                       jboolean enabled) {
    originalUiWanted.store(enabled == JNI_TRUE);
}
namespace {
jstring javaUtf8(JNIEnv *env, const std::string &text) {
    auto bytes = env->NewByteArray(jsize(text.size()));
    if (!bytes)
        return nullptr;
    env->SetByteArrayRegion(bytes, 0, jsize(text.size()), reinterpret_cast<const jbyte *>(text.data()));
    auto type = env->FindClass("java/lang/String");
    auto constructor = env->GetMethodID(type, "<init>", "([BLjava/lang/String;)V");
    auto encoding = env->NewStringUTF("UTF-8");
    auto result = static_cast<jstring>(env->NewObject(type, constructor, bytes, encoding));
    env->DeleteLocalRef(bytes);
    env->DeleteLocalRef(encoding);
    env->DeleteLocalRef(type);
    return result;
}
} // namespace
// One line for the world overlay: why the original interface is (not) on screen.
extern "C" JNIEXPORT jstring JNICALL Java_org_wowdecomp_android_MainActivity_originalUiInfo(JNIEnv *env, jclass) {
    std::string text;
    if (!originalUiWanted.load())
        text = "UI original: desligada (Configuração)";
    else if (originalUiStatus.load() == 2) {
        std::lock_guard<std::mutex> lock(originalUiErrorMutex);
        text = "UI original falhou: " + originalUiError;
    } else if (!wow::android::uiActive())
        text = "UI original: aguardando início";
    else
        text = "UI original: " + wow::android::uiDiagnostics();
    return javaUtf8(env, text);
}
// Archive files opened this run (debug export): recorded from the first app start.
extern void (*g_sfileOpenObserver)(const char *name);
namespace {
std::mutex openedFilesMutex;
std::set<std::string> openedFiles;
void recordOpenedFile(const char *name) {
    if (!name || !*name)
        return;
    std::string key(name);
    for (char &c : key)
        c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
    std::lock_guard<std::mutex> lock(openedFilesMutex);
    if (openedFiles.size() < 200000)
        openedFiles.insert(std::move(key));
}
struct OpenedFilesHook {
    OpenedFilesHook() { g_sfileOpenObserver = recordOpenedFile; }
} openedFilesHook;
} // namespace
extern "C" JNIEXPORT jobjectArray JNICALL Java_org_wowdecomp_android_MainActivity_openedArchiveFiles(JNIEnv *env, jclass) {
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(openedFilesMutex);
        names.assign(openedFiles.begin(), openedFiles.end());
    }
    jobjectArray array = env->NewObjectArray(jsize(names.size()), env->FindClass("java/lang/String"), nullptr);
    if (!array)
        return nullptr;
    for (size_t i = 0; i < names.size(); ++i) {
        jstring text = env->NewStringUTF(names[i].c_str());
        env->SetObjectArrayElement(array, jsize(i), text);
        env->DeleteLocalRef(text);
    }
    return array;
}
extern "C" JNIEXPORT jstring JNICALL Java_org_wowdecomp_android_MainActivity_originalUiLog(JNIEnv *env, jclass) {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(originalUiErrorMutex);
        if (!originalUiError.empty())
            text = "Falha: " + originalUiError + "\n";
    }
    return javaUtf8(env, text + wow::android::uiLogText());
}
// 0 off, 1 running (original FrameXML frames are drawn), 2 failed to start.
extern "C" JNIEXPORT jint JNICALL Java_org_wowdecomp_android_MainActivity_originalUiState(JNIEnv *, jclass) {
    if (!originalUiWanted.load())
        return 0;
    const int status = originalUiStatus.load();
    return status == 1 && !originalUiRunning.load() ? 0 : status;
}
