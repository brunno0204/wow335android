// Device-independent material operations, shared by original desktop builds
// and the native fixed-function GLES adapter. Model lighting/shadow integration
// remains in ShaderEffect.cpp.
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxMatrixStack.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CImVector.h"

namespace {
int32_t PackedColor(const CImVector &color) {
    int32_t value;
    std::memcpy(&value, &color, sizeof(value));
    return value;
}
} // namespace

// STATIC: 0x00D43008
int32_t CShaderEffect::s_useAlphaRef;

// STATIC: 0x00D4300C
float CShaderEffect::s_fogMul;

// STATIC: 0x00D43010
uint32_t CShaderEffect::s_shadowValue;

// STATIC: 0x00D43014
uint32_t CShaderEffect::s_usePcfFiltering;

// STATIC: 0x00D43018
uint32_t CShaderEffect::s_localLightCount;

// STATIC: 0x00D4301C
int32_t CShaderEffect::s_lightEnabled;

// STATIC: 0x00D43020
int32_t CShaderEffect::s_enableShaders;

// STATIC: 0x00D43024
CShaderEffect *CShaderEffect::s_curEffect;

// STATIC: 0x00D43028
C3Vector CShaderEffect::s_sunDiffuse;

// STATIC: 0x00D43034
C3Vector CShaderEffect::s_sunAmbient;

// STATIC: 0x00D43040
C3Vector CShaderEffect::s_sunDir;

// STATIC: 0x00D4304C
C3Vector CShaderEffect::s_sunSpec;

// STATIC: 0x00D43058
C4Vector CShaderEffect::s_fogColorAlphaRef;

// STATIC: 0x00D43068
C4Vector CShaderEffect::s_fogParams;

// STATIC: 0x00D43098
LocalLights CShaderEffect::s_localLights;

// VERIFY: 9049c785 manual
// OFFSET: 0x008728C0
void CShaderEffect::InitFixedFuncPass(const EGxTexOp *colorOps, const EGxTexOp *alphaOps, uint32_t count) {
#if !defined(_WIN32)
    if (count > 2 || (count && (!colorOps || !alphaOps)))
        throw std::invalid_argument("Invalid fixed material stage count");
    for (uint32_t i = 0; i < count; ++i)
        if (uint32_t(colorOps[i]) >= GxTexOps_Last || uint32_t(alphaOps[i]) >= GxTexOps_Last)
            throw std::invalid_argument("Invalid fixed material texture operation");
#endif
    this->m_fixedFuncPassCount = count;

    for (uint32_t i = 0; i < count; i++) {
        this->m_colorOps[i] = colorOps[i];
        this->m_alphaOps[i] = alphaOps[i];
    }
}

// VERIFY: 3d4e9331 auto
// OFFSET: 0x00872AD0
void CShaderEffect::SetUsePcfFiltering(uint32_t usePcfFiltering) {
    if (usePcfFiltering) {
        CShaderEffect::s_usePcfFiltering = 1;

        if (CShaderEffect::s_enableShaders) {
            return;
        }
    }

    CShaderEffect::s_usePcfFiltering = 0;
}

// VERIFY: b3663650 manual
// OFFSET: 0x00872B00
void CShaderEffect::UpdateWorldViewMatrix() {
    if (!CShaderEffect::s_enableShaders) {
        return;
    }

    C44Matrix view;
    C44Matrix world;
    GxXformView(view);

    CGxMatrixStack &worldStack = g_theGxDevicePtr->m_xforms[GxXform_World];
    world = worldStack.m_mtx[worldStack.m_level];

    C44Matrix worldView = world * view;
    worldView = worldView.Transpose();

    GxShaderConstantsSet(GxSh_Vertex, 31, reinterpret_cast<const float *>(&worldView), 4);
}

// VERIFY: 3063e016 auto
// OFFSET: 0x00872C10
void CShaderEffect::UpdateProjMatrix() {
    if (!CShaderEffect::s_enableShaders) {
        return;
    }

    C44Matrix projection;
    projection = g_theGxDevicePtr->m_projNative;

    if (g_theGxDevicePtr->DeviceApi() == GxApi_OpenGl) {
        projection.c0 = projection.c0 * -1.0f;
        projection.c1 = projection.c1 * -1.0f;
        projection.c2 = projection.c2 * -1.0f;
        projection.c3 = -1.0f * projection.c3;
    }

    projection = projection.Transpose();

    GxShaderConstantsSet(GxSh_Vertex, 2, reinterpret_cast<const float *>(&projection), 4);
}

// VERIFY: 326b299e auto
// OFFSET: 0x00872CE0
void CShaderEffect::InitShaderSystem(int32_t enableShaders, int32_t usePcfFiltering) {
#if !defined(_WIN32)
    if (enableShaders)
        throw std::runtime_error("Native legacy shader translation is not implemented");
#endif
    CShaderEffect::s_enableShaders = enableShaders;
    CShaderEffect::SetUsePcfFiltering(usePcfFiltering);
    CShaderEffect::s_fogMul = 1.0f;
    CShaderEffect::s_useAlphaRef = g_theGxDevicePtr->Caps().m_shaderAlphaRef;
}

// VERIFY: fd7f8aba manual
// OFFSET: 0x00872D30
void CShaderEffect::InitEffect(const char *vertexShader, const char *pixelShader) {
    memset(this->m_vertexShaders, 0, sizeof(this->m_vertexShaders));
    memset(this->m_pixelShaders, 0, sizeof(this->m_pixelShaders));
    this->m_fixedFuncPassCount = 0;

    if (!CShaderEffect::s_enableShaders || !vertexShader || !pixelShader) {
        return;
    }

    g_theGxDevicePtr->ShaderCreate(this->m_vertexShaders, GxSh_Vertex, "Shaders\\Vertex", vertexShader,
                                   sizeof(this->m_vertexShaders) / sizeof(this->m_vertexShaders[0]));
    g_theGxDevicePtr->ShaderCreate(this->m_pixelShaders, GxSh_Pixel, "Shaders\\Pixel", pixelShader,
                                   sizeof(this->m_pixelShaders) / sizeof(this->m_pixelShaders[0]));
}

// VERIFY: 9b6a2325 manual
// OFFSET: 0x00872DE0
uint32_t CShaderEffect::SelectShadowShader() {
    if (!(CShaderEffect::s_fogColorAlphaRef.w > 0.0f) ||
        (g_theGxDevicePtr->Caps().m_shaderAlphaRef && !CShaderEffect::s_shadowValue)) {
        return CShaderEffect::s_shadowValue + 4 * CShaderEffect::s_usePcfFiltering;
    }

    return CShaderEffect::s_shadowValue + 4 * (CShaderEffect::s_usePcfFiltering + 2);
}

// VERIFY: fd36fa66 manual
// OFFSET: 0x00872F90
void CShaderEffect::SetCurrent() {
    CShaderEffect::s_curEffect = this;

    if (CShaderEffect::s_enableShaders || !this->m_fixedFuncPassCount) {
        return;
    }

    for (uint32_t i = 0; i < this->m_fixedFuncPassCount; i++) {
        GxRsSet(static_cast<EGxRenderState>(GxRs_ColorOp0 + i), this->m_colorOps[i]);
        GxRsSet(static_cast<EGxRenderState>(GxRs_AlphaOp0 + i), this->m_alphaOps[i]);
    }
}

// VERIFY: 0344992e manual
// OFFSET: 0x00873060
void CShaderEffect::SetShaders(uint32_t vertexPermute, uint32_t pixelPermute) {
    int32_t useAlphaRef = 1;

    if (CShaderEffect::s_enableShaders) {
        g_theGxDevicePtr->RsSet(GxRs_VertexShader,
                                CShaderEffect::s_curEffect->m_vertexShaders[vertexPermute]);
        g_theGxDevicePtr->RsSet(GxRs_PixelShader, CShaderEffect::s_curEffect->m_pixelShaders[pixelPermute]);
        useAlphaRef = (pixelPermute & 8) == 0;
    }

    if (useAlphaRef == CShaderEffect::s_useAlphaRef) {
        return;
    }

    CShaderEffect::s_useAlphaRef = useAlphaRef;

    if (useAlphaRef) {
        float alphaRef = CShaderEffect::s_fogColorAlphaRef.w * 255.0f;
        GxRsSet(GxRs_AlphaRef, static_cast<int32_t>(alphaRef));
    } else {
        GxShaderConstantsSet(GxSh_Pixel, 2, &CShaderEffect::s_fogColorAlphaRef.x, 1);
        GxRsSet(GxRs_AlphaRef, 0);
    }
}

// VERIFY: f5c47bbc auto
// OFFSET: 0x00873160
void CShaderEffect::SetDefaultShaders(uint32_t lightCount) {
    if (!CShaderEffect::s_enableShaders) {
        return;
    }

    uint32_t shadowValue = CShaderEffect::s_shadowValue > 2 ? 2 : CShaderEffect::s_shadowValue;
    uint32_t lights = lightCount > 2 ? 2 : lightCount;

    uint32_t vertexPermute = CShaderEffect::s_lightEnabled +
                             2 * (CShaderEffect::s_localLightCount + 5 * (lights + 3 * shadowValue));

    CShaderEffect::SetShaders(vertexPermute, CShaderEffect::SelectShadowShader());
}

// VERIFY: 71c6c864 manual
// OFFSET: 0x008731C0
void CShaderEffect::SetLightingEnabled(int32_t enabled) {
    CShaderEffect::s_lightEnabled = enabled;

    if (!CShaderEffect::s_enableShaders) {
        GxRsSet(GxRs_Lighting, enabled);
    }
}

// VERIFY: 26a9b9a3 manual
// OFFSET: 0x00873210
void CShaderEffect::SetFogParams(float start, float end, float density, const CImVector &color) {
    if (CShaderEffect::s_enableShaders) {
        CShaderEffect::s_fogColorAlphaRef.x = color.r * (1.0f / 255.0f);
        CShaderEffect::s_fogColorAlphaRef.y = color.g * (1.0f / 255.0f);
        CShaderEffect::s_fogColorAlphaRef.z = (1.0f / 255.0f) * color.b;

        double scale = 1.0 / (end - start);
        CShaderEffect::s_fogParams.x = -(CShaderEffect::s_fogMul * scale);
        CShaderEffect::s_fogParams.y = end * scale;
        CShaderEffect::s_fogParams.z = density;
        CShaderEffect::s_fogParams.w = 0.0f;

        if (!g_theGxDevicePtr->Caps().unk_0134) {
            GxShaderConstantsSet(GxSh_Pixel, 2, &CShaderEffect::s_fogColorAlphaRef.x, 1);
            return;
        }
    } else {
        GxRsSet(GxRs_FogStart, start);
        GxRsSet(GxRs_FogEnd, end);
    }

    GxRsSet(GxRs_FogColor, PackedColor(color));
}

// VERIFY: af8a3952 manual
// OFFSET: 0x00873390
void CShaderEffect::SetFogEnabled(int32_t enabled) {
    if (enabled && g_theGxDevicePtr->MasterEnable(GxMasterEnable_Fog)) {
        if (CShaderEffect::s_enableShaders) {
            GxShaderConstantsSet(GxSh_Vertex, 30, &CShaderEffect::s_fogParams.x, 1);

            if (!g_theGxDevicePtr->Caps().unk_0138) {
                return;
            }
        }

        GxRsSet(GxRs_Fog, 1);
        return;
    }

    if (!CShaderEffect::s_enableShaders || g_theGxDevicePtr->Caps().unk_0138) {
        GxRsSet(GxRs_Fog, 0);
    } else {
        float fogParams[4] = {0.0f, 1.0f, 1.0f, 0.0f};
        GxShaderConstantsSet(GxSh_Vertex, 30, fogParams, 1);
    }
}

// VERIFY: c6b67cc5 manual
// OFFSET: 0x00873480
void CShaderEffect::SetTexMtx_Identity(uint32_t stage) {
#if !defined(_WIN32)
    if (stage >= 8 || stage >= uint32_t(g_theGxDevicePtr->Caps().m_numTmus))
        throw std::invalid_argument("Invalid native material texture stage");
#endif
    if (CShaderEffect::s_enableShaders) {
        float matrix[8] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        GxShaderConstantsSet(GxSh_Vertex, 2 * stage + 6, matrix, 2);
        return;
    }

    GxRsSet(static_cast<EGxRenderState>(GxRs_TextureShader0 + stage), 0);
    g_theGxDevicePtr->m_xforms[stage].Identity();
    GxRsSet(static_cast<EGxRenderState>(GxRs_TexGen0 + stage), 0);
}

// VERIFY: d55e347c manual
// OFFSET: 0x00873620
void CShaderEffect::SetTexMtx(const C44Matrix &matrix, uint32_t stage) {
#if !defined(_WIN32)
    if (stage >= 8 || stage >= uint32_t(g_theGxDevicePtr->Caps().m_numTmus))
        throw std::invalid_argument("Invalid native material texture stage");
#endif
    if (CShaderEffect::s_enableShaders) {
        float constants[8] = {matrix.a0, matrix.b0, matrix.c0, matrix.d0,
                              matrix.a1, matrix.b1, matrix.c1, matrix.d1};
        GxShaderConstantsSet(GxSh_Vertex, 2 * stage + 6, constants, 2);
        return;
    }

    GxRsSet(static_cast<EGxRenderState>(GxRs_TextureShader0 + stage), 1);

    CGxMatrixStack &stack = g_theGxDevicePtr->m_xforms[stage];
    stack.m_dirty = 1;
    stack.m_flags[stack.m_level] &= ~CGxMatrixStack::F_Identity;
    stack.m_mtx[stack.m_level] = matrix;

    GxRsSet(static_cast<EGxRenderState>(GxRs_TexGen0 + stage), 0);
}

// VERIFY: f365063d manual
// OFFSET: 0x00873900
void CShaderEffect::SetDiffuse(const C4Vector &color) {
    if (CShaderEffect::s_enableShaders) {
        GxShaderConstantsSet(GxSh_Vertex, 28, &color.x, 1);
        return;
    }

    float r = color.x < 0.0f ? 0.0f : (color.x >= 1.0f ? 1.0f : color.x);
    float g = color.y < 0.0f ? 0.0f : (color.y >= 1.0f ? 1.0f : color.y);
    float b = color.z < 0.0f ? 0.0f : (color.z >= 1.0f ? 1.0f : color.z);
    float a = color.w < 0.0f ? 0.0f : (color.w >= 1.0f ? 1.0f : color.w);

    CImVector diffuse;
    diffuse.r = static_cast<uint8_t>(r * 255.0f);
    diffuse.g = static_cast<uint8_t>(g * 255.0f);
    diffuse.b = static_cast<uint8_t>(b * 255.0f);
    diffuse.a = static_cast<uint8_t>(a * 255.0f);

    GxRsSet(GxRs_MatDiffuse, PackedColor(diffuse));
}

// VERIFY: d0cdacd9 manual
// OFFSET: 0x00873A50
void CShaderEffect::SetEmissive(const C4Vector &color) {
    if (CShaderEffect::s_enableShaders) {
        GxShaderConstantsSet(GxSh_Vertex, 29, &color.x, 1);
        return;
    }

    float r = color.x < 0.0f ? 0.0f : (color.x >= 1.0f ? 1.0f : color.x);
    float g = color.y < 0.0f ? 0.0f : (color.y >= 1.0f ? 1.0f : color.y);
    float b = color.z < 0.0f ? 0.0f : (color.z >= 1.0f ? 1.0f : color.z);
    float a = color.w < 0.0f ? 0.0f : (color.w >= 1.0f ? 1.0f : color.w);

    CImVector emissive;
    emissive.r = static_cast<uint8_t>(r * 255.0f);
    emissive.g = static_cast<uint8_t>(g * 255.0f);
    emissive.b = static_cast<uint8_t>(b * 255.0f);
    emissive.a = static_cast<uint8_t>(a * 255.0f);

    GxRsSet(GxRs_MatEmissive, PackedColor(emissive));
}

// VERIFY: 7e25358b manual
// OFFSET: 0x00873BA0
void CShaderEffect::SetAlphaRef(float alphaRef) {
    CShaderEffect::s_fogColorAlphaRef.w = alphaRef;

    if (!CShaderEffect::s_useAlphaRef) {
        GxShaderConstantsSet(GxSh_Pixel, 2, &CShaderEffect::s_fogColorAlphaRef.x, 1);
        return;
    }

    float value = alphaRef * 255.0f;
    GxRsSet(GxRs_AlphaRef, static_cast<int32_t>(value));
}

// VERIFY: a90cae3f manual
// OFFSET: 0x00873C20
CShaderEffect::~CShaderEffect() {
#if !defined(_WIN32)
    if (s_curEffect == this)
        s_curEffect = nullptr;
#endif
    if (!CShaderEffect::s_enableShaders) {
        return;
    }

    for (uint32_t i = 0; i < sizeof(this->m_vertexShaders) / sizeof(this->m_vertexShaders[0]); i++) {
        if (this->m_vertexShaders[i]) {
            g_theGxDevicePtr->ShaderDestroy(this->m_vertexShaders[i]);
        }

        this->m_vertexShaders[i] = nullptr;
    }

    for (uint32_t i = 0; i < sizeof(this->m_pixelShaders) / sizeof(this->m_pixelShaders[0]); i++) {
        if (this->m_pixelShaders[i]) {
            g_theGxDevicePtr->ShaderDestroy(this->m_pixelShaders[i]);
        }

        this->m_pixelShaders[i] = nullptr;
    }
}

// VERIFY: fd00dfa0 manual
// OFFSET: 0x00873EE0
void CShaderEffect::SetAlphaRefDefault() {
    float alphaRef =
        CGxDevice::s_alphaRef[g_theGxDevicePtr->m_appRenderStates[GxRs_BlendingMode].m_value.m_data.i[0]] *
        (1.0f / 255.0f);
    CShaderEffect::SetAlphaRef(alphaRef);
}

// VERIFY: 47270730 auto
// OFFSET: 0x00873F30
CShaderEffect::CShaderEffect() {
#if !defined(_WIN32)
    m_fixedFuncPassCount = 0;
    for (auto &op : m_colorOps)
        op = GxTexOp_Mod;
    for (auto &op : m_alphaOps)
        op = GxTexOp_Mod;
    std::memset(m_vertexShaders, 0, sizeof(m_vertexShaders));
    std::memset(m_pixelShaders, 0, sizeof(m_pixelShaders));
#endif
}

// VERIFY: f66fd164 auto
// OFFSET: 0x007A8A60
LocalLights::LocalLights() {
    for (uint32_t i = 0; i < 4; i++) {
        this->m_color[i].x = 0.0f;
        this->m_color[i].y = 0.0f;
        this->m_color[i].z = 0.0f;
        this->m_color[i].w = 0.0f;
    }

    for (uint32_t i = 0; i < 4; i++) {
        this->m_position[i].x = 0.0f;
        this->m_position[i].y = 0.0f;
        this->m_position[i].z = 0.0f;
        this->m_position[i].w = 0.0f;
    }

    for (uint32_t i = 0; i < 4; i++) {
        this->m_constantAttenuation[i] = 0.0f;
        this->m_linearAttenuation[i] = 0.0f;
        this->m_quadraticAttenuation[i] = 0.0f;
    }
}
