#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/CGxDevice/CGxShader.h"
#include "Engine/Source/Gx/CGxMatrixStack.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#include "Engine/Source/Tempest/CImVector.h"

// VERIFY: ce1f9620 manual
// OFFSET: 0x00873550
void CShaderEffect::SetTexMtx_SphereMap(uint32_t stage) {
    if (CShaderEffect::s_enableShaders) {
        float matrix[8] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
        GxShaderConstantsSet(GxSh_Vertex, 2 * stage + 6, matrix, 2);
        return;
    }

    GxRsSet(static_cast<EGxRenderState>(GxRs_TextureShader0 + stage), 0);
    g_theGxDevicePtr->m_xforms[stage].Identity();
    GxRsSet(static_cast<EGxRenderState>(GxRs_TexGen0 + stage), 6);
}

// VERIFY: 571cb58b manual
// OFFSET: 0x00873730
void CShaderEffect::SetTexMtx_TexGen(EGxTexGen texGen, const C44Matrix& matrix, uint32_t stage) {
    if (CShaderEffect::s_enableShaders) {
        C44Matrix view;
        GxXformView(view);

        float swap = view.a1;
        view.a1 = view.b0;
        view.b0 = swap;
        swap = view.a2;
        view.a2 = view.c0;
        view.c0 = swap;
        swap = view.b2;
        view.b2 = view.c1;
        view.c1 = swap;
        view.d0 = view.d0 * -1.0f;
        view.d1 = view.d1 * -1.0f;
        view.d2 = -1.0f * view.d2;

        C44Matrix texMatrix = view * matrix;
        float constants[8] = { texMatrix.a0, texMatrix.b0, texMatrix.c0, texMatrix.d0, texMatrix.a1, texMatrix.b1, texMatrix.c1, texMatrix.d1 };
        GxShaderConstantsSet(GxSh_Vertex, 2 * stage + 6, constants, 2);
        return;
    }

    GxRsSet(static_cast<EGxRenderState>(GxRs_TextureShader0 + stage), 1);

    CGxMatrixStack& stack = g_theGxDevicePtr->m_xforms[stage];
    stack.m_dirty = 1;
    stack.m_flags[stack.m_level] &= ~CGxMatrixStack::F_Identity;
    stack.m_mtx[stack.m_level] = matrix;

    GxRsSet(static_cast<EGxRenderState>(GxRs_TexGen0 + stage), GxTexGen_World);
}
