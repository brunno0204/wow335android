// 3D model frames of the original interface (PlayerModel, DressUpModel, TabardModel): the
// engine's CSimpleModel with the unit's loaded world model drawn into the frame rectangle
// (the client's CGCharacterModelBase renders through CM2Scene, which this port does not have).
#include "UiModelFrames.h"
#include "WorldScene.h"
#include "Engine/Source/Base/Coordinate.h"
#include "Engine/Source/Frame/CRenderBatch.h"
#include "Engine/Source/Frame/CSimpleModel.h"
#include "Engine/Source/Frame/CSimpleTexture.h"
#include "Engine/Source/Services/Texture.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include <cstdlib>
#include <cstring>
#include "WorldLink.h"
#include "UiRuntime.h"
#include <cstdio>
#include <string>
#include <mutex>
#include <new>
#include <typeinfo>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <unordered_map>
#include <vector>

namespace wow::android {
namespace {
struct Binding {
    uint64_t guid = 0;  // 0 = local player
    bool shown = false; // a unit is set
    bool portrait = false;
    float rotation = 0, zoom = 1;
};
std::unordered_map<const void *, Binding> bindings;

class AndroidModel : public CSimpleModel {
  public:
    explicit AndroidModel(CSimpleFrame *parent) : CSimpleModel(parent) {}
    ~AndroidModel() override { bindings.erase(this); }
    void OnFrameRender(CRenderBatch *batch, uint32_t layer) override {
        CSimpleModel::OnFrameRender(batch, layer);
        const auto found = bindings.find(this);
        if (layer == 2 && found != bindings.end() && found->second.shown)
            batch->QueueCallback(&AndroidModel::render, this);
    }
    static void render(void *arg) {
        auto *frame = static_cast<AndroidModel *>(arg);
        const auto found = bindings.find(frame);
        if (found == bindings.end())
            return;
        CRect rect{0, 0, 0, 0};
        if (!frame->GetRect(&rect))
            return;
        const float width = rect.maxX - rect.minX, height = rect.maxY - rect.minY;
        if (width <= 0 || height <= 0)
            return;
        float minX, maxX, minY, maxY, minZ, maxZ;
        GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);
        float x0, y0, x1, y1;
        DDCToNDC(rect.minX, rect.minY, &x0, &y0);
        DDCToNDC(rect.maxX, rect.maxY, &x1, &y1);
        auto clamp01 = [](float v) { return v < 0 ? 0.f : (v > 1 ? 1.f : v); };
        x0 = clamp01(x0), x1 = clamp01(x1), y0 = clamp01(y0), y1 = clamp01(y1);
        if (x1 - x0 < 1e-5f || y1 - y0 < 1e-5f)
            return;
        g_theGxDevicePtr->RsPush();
        GxRsSet(GxRs_ScissorTest, 0); // a UI clip rectangle may still be set
        GxXformPush(GxXform_World);
        GxXformPush(GxXform_View);
        GxXformPush(GxXform_Projection);
        GxXformPush(GxXform_Tex0);
        GxXformPush(GxXform_Tex1);
        GxXformSetViewport(x0 * maxX, x1 * maxX, y0 * maxY, y1 * maxY, 0.0f, 1.0f);
        GxSceneClear(GxBufferBit_Depth, CImVector{0, 0, 0, 255});
        const auto &b = found->second;
        worldSceneDrawUnitView(b.guid, b.rotation + frame->m_facing, width / height, b.portrait, b.zoom);
        GxXformPop(GxXform_Tex1);
        GxXformPop(GxXform_Tex0);
        GxXformPop(GxXform_Projection);
        GxXformPop(GxXform_View);
        GxXformPop(GxXform_World);
        g_theGxDevicePtr->RsPop();
        GxXformSetViewport(minX, maxX, minY, maxY, minZ, maxZ);
        CShaderEffect::UpdateProjMatrix();
    }
};
static_assert(sizeof(AndroidModel) == sizeof(CSimpleModel), "allocated from CSimpleModel::s_allocator");

AndroidModel *self(lua_State *L) {
    auto *frame = static_cast<CSimpleModel *>(FrameScript_GetObjectThis(L, CSimpleModel::GetObjectType()));
    return frame ? static_cast<AndroidModel *>(frame) : nullptr;
}
bool isAndroidModel(CSimpleModel *frame) { return frame && dynamic_cast<AndroidModel *>(frame); }
uint64_t guidArg(lua_State *L, int index) {
    const char *text = lua_tostring(L, index);
    if (!text || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
        return 0;
    return std::strtoull(text + 2, nullptr, 16);
}
// __AndroidModelSetUnit(frame, guid|nil[, portrait]): nil clears the model.
int lSetUnit(lua_State *L) {
    auto *frame = self(L);
    if (!isAndroidModel(frame))
        return 0;
    auto &b = bindings[frame];
    b.shown = lua_isstring(L, 2) != 0;
    b.guid = guidArg(L, 2);
    b.portrait = lua_toboolean(L, 3) != 0;
    return 0;
}
int lSetRotation(lua_State *L) {
    auto *frame = self(L);
    if (isAndroidModel(frame) && lua_isnumber(L, 2))
        bindings[frame].rotation = float(lua_tonumber(L, 2));
    return 0;
}
int lGetRotation(lua_State *L) {
    auto *frame = self(L);
    const auto found = frame ? bindings.find(frame) : bindings.end();
    lua_pushnumber(L, found != bindings.end() ? found->second.rotation : 0);
    return 1;
}
int lSetZoom(lua_State *L) {
    auto *frame = self(L);
    if (isAndroidModel(frame) && lua_isnumber(L, 2))
        bindings[frame].zoom = float(lua_tonumber(L, 2));
    return 0;
}
// ---- 3D portraits (SetPortraitTexture): the unit's model rendered head-on into a corner of
// the frame before the world is drawn, read back, masked round and kept as a texture.
constexpr uint32_t kPortrait = 64;
struct Portrait {
    CTexture *texture = nullptr;
    std::vector<uint32_t> pixels = std::vector<uint32_t>(kPortrait * kPortrait, 0);
    bool pending = false, ready = false;
    uint64_t retryMs = 0;
    int attempts = 0;
};
std::map<uint64_t, Portrait> portraits;
std::unordered_map<CSimpleTexture *, uint64_t> portraitRegions; // region -> unit shown in it
uint64_t steadyNowMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
void portraitCallback(EGxTexCommand command, uint32_t, uint32_t, uint32_t, uint32_t, void *arg, uint32_t &stride,
                      const void *&texels) {
    if (command == GxTex_Latch) {
        stride = kPortrait * 4;
        texels = static_cast<Portrait *>(arg)->pixels.data();
    }
}
void showPortrait(CSimpleTexture *region, Portrait &portrait) {
    if (!portrait.texture)
        return;
    region->SetTexture(portrait.texture);
    CRect coords{0, 0, 0, 0};
    coords.minX = 0;
    coords.maxX = 1;
    coords.minY = 0;
    coords.maxY = 1;
    region->SetTexCoord(coords);
}
uint64_t worldPlayerGuid() {
    std::lock_guard<std::mutex> link(worldLinkMutex());
    return worldLink().guid;
}
// __AndroidPortraitRequest(texture, guid, unit) -> true when a 3D portrait is already shown.
int lPortraitRequest(lua_State *L) {
    auto *region = static_cast<CSimpleTexture *>(FrameScript_GetObjectThis(L, CSimpleTexture::GetObjectType()));
    uint64_t guid = guidArg(L, 2);
    // The local player is keyed 0 (the hero model), whether or not its GUID is known yet.
    const char *unit = lua_tostring(L, 3);
    if (unit && (std::strcmp(unit, "player") == 0 || std::strcmp(unit, "PLAYER") == 0))
        guid = 0;
    else if (guid && guid == worldPlayerGuid())
        guid = 0;
    if (!region || (!guid && !(unit && (std::strcmp(unit, "player") == 0 || std::strcmp(unit, "PLAYER") == 0)))) {
        if (region)
            portraitRegions.erase(region);
        return 0;
    }
    portraitRegions[region] = guid;
    auto &portrait = portraits[guid];
    if (!portrait.pending) {
        portrait.pending = true;
        portrait.attempts = 0;
        portrait.retryMs = 0;
    }
    if (portrait.ready) {
        showPortrait(region, portrait);
        lua_pushboolean(L, 1);
        return 1;
    }
    return 0;
}
} // namespace

bool uiRenderPortraits() {
    const uint64_t now = steadyNowMs();
    int rendered = 0;
    for (auto &[guid, portrait] : portraits) {
        if (!portrait.pending || now < portrait.retryMs || rendered >= 2)
            continue;
        CRect window{0, 0, 0, 0};
        g_theGxDevicePtr->CapsWindowSizeInScreenCoords(window);
        const float width = window.maxX - window.minX, height = window.maxY - window.minY;
        if (width < kPortrait * 2 || height < kPortrait * 2)
            return rendered > 0;
        float minX, maxX, minY, maxY, minZ, maxZ;
        GxXformViewport(minX, maxX, minY, maxY, minZ, maxZ);
        if (std::getenv("WOWPORT_PORTRAIT_DEBUG"))
            std::fprintf(stderr, "PORTRAIT viewport %.3f %.3f %.3f %.3f window %.0f %.0f\n", minX, maxX, minY, maxY, width, height);
        g_theGxDevicePtr->RsPush();
        GxRsSet(GxRs_ScissorTest, 0); // a UI clip rectangle may still be set
        GxXformPush(GxXform_World);
        GxXformPush(GxXform_View);
        GxXformPush(GxXform_Projection);
        // Top-left kPortrait x kPortrait pixels (viewport y is from the bottom).
        GxXformSetViewport(0.0f, kPortrait / width, 1.0f - kPortrait / height, 1.0f, 0.0f, 1.0f);
        GxSceneClear(GxBufferBit_Color | GxBufferBit_Depth, CImVector{0, 0, 0, 255});
        const bool drawn = worldSceneDrawUnitView(guid, 0, 1, true, 1);
        GxXformPop(GxXform_Projection);
        GxXformPop(GxXform_View);
        GxXformPop(GxXform_World);
        g_theGxDevicePtr->RsPop();
        GxXformSetViewport(minX, maxX, minY, maxY, minZ, maxZ);
        ++rendered;
        if (!drawn && portrait.attempts < 2)
            uiLog(std::string("retrato ") + (guid ? "unidade" : "jogador") + ": modelo ainda não carregado");
        if (!drawn) {
            // Model not loaded yet (or out of range): retry for a while.
            // The player's own model can take long to load: keep trying, slower after 30 s.
            portrait.retryMs = now + (++portrait.attempts > 30 ? 3000 : 1000);
            continue;
        }
        CiRect rect{0, 0, int32_t(kPortrait), int32_t(kPortrait)};
        rect.minX = 0;
        rect.minY = 0;
        rect.maxX = int32_t(kPortrait);
        rect.maxY = int32_t(kPortrait);
        TSGrowableArray<CImVector> pixels;
        GxReadPixels(rect, pixels);
        if (pixels.Count() < kPortrait * kPortrait) {
            portrait.retryMs = now + 2000;
            continue;
        }
        if (const char *dump = std::getenv("WOWPORT_PORTRAIT_WIDE")) { // desktop harness only
            CiRect wide{0, 0, 160, 160};
            TSGrowableArray<CImVector> big;
            GxReadPixels(wide, big);
            if (FILE *f = std::fopen(dump, "wb")) {
                std::fprintf(f, "P6 160 160 255\n");
                for (uint32_t i = 0; i < big.Count(); ++i) {
                    const uint8_t rgb[3]{big[i].r, big[i].g, big[i].b};
                    std::fwrite(rgb, 1, 3, f);
                }
                std::fclose(f);
            }
        }
        if (const char *dump = std::getenv("WOWPORT_PORTRAIT_DUMP")) { // desktop harness only
            if (FILE *f = std::fopen((std::string(dump) + (guid ? "-unit.ppm" : "-player.ppm")).c_str(), "wb")) {
                std::fprintf(f, "P6 %u %u 255\n", kPortrait, kPortrait);
                for (uint32_t i = 0; i < kPortrait * kPortrait; ++i) {
                    const uint8_t rgb[3]{pixels[i].r, pixels[i].g, pixels[i].b};
                    std::fwrite(rgb, 1, 3, f);
                }
                std::fclose(f);
            }
        }
        size_t lit = 0;
        for (uint32_t i = 0; i < kPortrait * kPortrait; ++i)
            lit += pixels[i].r > 12 || pixels[i].g > 12 || pixels[i].b > 12;
        if (portrait.attempts < 1000) {
            char line[160];
            std::snprintf(line, sizeof(line), "retrato %s: tentativa %d, %zu/%u pixels com imagem",
                          guid ? "unidade" : "jogador", portrait.attempts, lit, kPortrait * kPortrait);
            if (portrait.attempts < 3 || lit > 40)
                uiLog(line + (worldSceneViewError().empty() ? std::string() : " · " + worldSceneViewError()));
        }
        if (lit <= 40) {
            // Nothing drawn (model not ready, culled or clipped): keep the icon and try again.
            portrait.retryMs = now + (++portrait.attempts > 30 ? 3000 : 1000);
            continue;
        }
        const float center = kPortrait / 2.0f;
        for (uint32_t y = 0; y < kPortrait; ++y)
            for (uint32_t x = 0; x < kPortrait; ++x) {
                const CImVector &c = pixels[y * kPortrait + x];
                const float dx = (x + .5f - center) / center, dy = (y + .5f - center) / center;
                const float edge = std::clamp((1.0f - std::sqrt(dx * dx + dy * dy)) * center, 0.0f, 1.0f);
                portrait.pixels[y * kPortrait + x] =
                    uint32_t(edge * 255) << 24 | uint32_t(c.r) << 16 | uint32_t(c.g) << 8 | c.b;
            }
        if (!portrait.texture) {
            CGxTexFlags flags(GxTex_Linear, 0, 0, 0, 0, 0, 1);
            portrait.texture = TextureCreate(kPortrait, kPortrait, GxTex_Argb8888, GxTex_Argb8888, flags, &portrait,
                                             portraitCallback, "AndroidPortrait", 0);
        } else if (CGxTex *gx = TextureGetGxTex(portrait.texture, 1, nullptr))
            GxTexUpdate(gx, 0, 0, int32_t(kPortrait), int32_t(kPortrait), 1);
        portrait.pending = false;
        portrait.ready = true;
        for (auto &[region, shown] : portraitRegions)
            if (shown == guid)
                showPortrait(region, portrait);
    }
    return rendered > 0;
}

CSimpleFrame *createUiModelFrame(CSimpleFrame *parent) {
    void *data = CSimpleModel::s_allocator.GetData(0, typeid(CSimpleModel).name(), SLOG_OBJECT);
    return data ? new (data) AndroidModel(parent) : nullptr;
}
void uiModelFramesRegister() {
    FrameScript_RegisterFunction("__AndroidModelSetUnit", lSetUnit);
    FrameScript_RegisterFunction("__AndroidModelSetRotation", lSetRotation);
    FrameScript_RegisterFunction("__AndroidModelGetRotation", lGetRotation);
    FrameScript_RegisterFunction("__AndroidModelSetZoom", lSetZoom);
    FrameScript_RegisterFunction("__AndroidPortraitRequest", lPortraitRequest);
}
} // namespace wow::android
