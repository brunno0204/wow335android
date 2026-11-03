#include <cmath>
#include "Engine/Source/ShaderEffect/ShaderEffect.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Model2/M2Light.h"
#include "Engine/Source/Tempest/C44Matrix.h"
#if defined(_WIN32)
#include "Engine/Source/ShaderEffect/ShadowCache.h"
#else
#include <stdexcept>
#endif

// VERIFY: be9bb4f7 manual
// OFFSET: 0x00872900
void CShaderEffect::ComputeLocalLights(LocalLights* localLights, uint32_t lightCount, CM2Light** lights, const C3Vector* origin) {
#if !defined(_WIN32)
    if (!localLights || lightCount > 4 || (lightCount && !lights))
        throw std::invalid_argument("Invalid local light array");
    for (uint32_t i = 0; i < lightCount; ++i)
        if (!lights[i]) throw std::invalid_argument("Null local light");
    *localLights = LocalLights();
#endif
    uint32_t i = 0;

    if (lightCount) {
        C44Matrix view;

        if (origin) {
            GxXformView(view);
        }

        for (; i < lightCount; i++) {
            CM2Light* light = lights[i];

            if (light->m_type == M2LIGHT_POINT) {
                localLights->m_color[i].x = light->m_dirColor.x;
                localLights->m_color[i].y = light->m_dirColor.y;
                localLights->m_color[i].z = light->m_dirColor.z;
                localLights->m_color[i].w = 1.0f;

                C3Vector position;

                if (origin) {
                    position = (light->m_pos - *origin) * view;
                } else {
                    position = light->m_viewPos;
                }

                localLights->m_position[i].x = position.x;
                localLights->m_position[i].y = position.y;
                localLights->m_position[i].z = position.z;
                localLights->m_position[i].w = 1.0f;

                localLights->m_constantAttenuation[i] = light->m_constantAttenuation;
                localLights->m_linearAttenuation[i] = light->m_linearAttenuation;
                localLights->m_quadraticAttenuation[i] = light->m_quadraticAttenuation;
            } else {
                localLights->m_color[i].x = 0.0f;
                localLights->m_color[i].y = 0.0f;
                localLights->m_color[i].z = 0.0f;
                localLights->m_color[i].w = 0.0f;
            }
        }
    }

    for (; i < 4; i++) {
        localLights->m_color[i].x = 0.0f;
        localLights->m_color[i].y = 0.0f;
        localLights->m_color[i].z = 0.0f;
        localLights->m_color[i].w = 0.0f;
    }
}

// VERIFY: ae00ba32 manual
// OFFSET: 0x00872E40
void CShaderEffect::SetGlobalLighting(CM2Light* light) {
#if !defined(_WIN32)
    if (!light) throw std::invalid_argument("Null global light");
    if (s_enableShaders) throw std::runtime_error("Native game shaders are not implemented");
#endif
    CShaderEffect::s_sunDir = light->m_dir;
    CShaderEffect::s_sunSpec = light->m_specColor;

    C44Matrix view;
    GxXformView(view);

    C3Vector sunDir;
    sunDir.x = view.c0 * CShaderEffect::s_sunDir.z + view.b0 * CShaderEffect::s_sunDir.y + view.a0 * CShaderEffect::s_sunDir.x;
    sunDir.y = CShaderEffect::s_sunDir.z * view.c1 + CShaderEffect::s_sunDir.y * view.b1 + CShaderEffect::s_sunDir.x * view.a1;
    sunDir.z = CShaderEffect::s_sunDir.z * view.c2 + CShaderEffect::s_sunDir.y * view.b2 + CShaderEffect::s_sunDir.x * view.a2;
    CShaderEffect::s_sunDir = sunDir;

    if (CShaderEffect::s_enableShaders) {
        C4Vector sunSpec;
        sunSpec.x = CShaderEffect::s_sunSpec.x;
        sunSpec.y = CShaderEffect::s_sunSpec.y;
        sunSpec.z = CShaderEffect::s_sunSpec.z;
        sunSpec.w = 14.0f;

        C4Vector sunDirConst;
        sunDirConst.x = CShaderEffect::s_sunDir.x;
        sunDirConst.y = CShaderEffect::s_sunDir.y;
        sunDirConst.z = CShaderEffect::s_sunDir.z;
        sunDirConst.w = CShaderEffect::s_sunSpec.x;

        GxShaderConstantsSet(GxSh_Vertex, 12, &sunDirConst.x, 1);
        GxShaderConstantsSet(GxSh_Vertex, 13, &sunSpec.x, 1);
    }
}

// VERIFY: 17775013 manual
// OFFSET: 0x00873CA0
void CShaderEffect::SetLocalLighting(CM2Lighting* lighting, int32_t enabled, const C3Vector* origin) {
#if !defined(_WIN32)
    if (s_enableShaders) throw std::runtime_error("Native game shaders are not implemented");
    if (enabled && !lighting) throw std::invalid_argument("Enabled model lighting needs state");
    if (lighting) {
        if (lighting->m_lightCount > 4) throw std::invalid_argument("Too many model lights");
        for (uint32_t i = 0; i < lighting->m_lightCount; ++i)
            if (!lighting->m_lights[i]) throw std::invalid_argument("Null model light");
    }
#endif
    CShaderEffect::s_lightEnabled = enabled;

    if (!CShaderEffect::s_enableShaders) {
        GxRsSet(GxRs_Lighting, enabled);
    }

    CShaderEffect::s_localLightCount = lighting ? lighting->m_lightCount : 0;

    if (!CShaderEffect::s_enableShaders) {
        if (enabled) {
            lighting->SetupGxLights(origin);
        }

        return;
    }

    if (!enabled) {
        return;
    }

    CShaderEffect::s_sunDir = lighting->m_sunDir;

    if (CShaderEffect::s_sunDir.x != 0.0f || CShaderEffect::s_sunDir.y != 0.0f || CShaderEffect::s_sunDir.z != 0.0f) {
        float scale = 1.0 / sqrt(CShaderEffect::s_sunDir.z * CShaderEffect::s_sunDir.z + CShaderEffect::s_sunDir.y * CShaderEffect::s_sunDir.y + CShaderEffect::s_sunDir.x * CShaderEffect::s_sunDir.x);
        CShaderEffect::s_sunDir.x = CShaderEffect::s_sunDir.x * scale;
        CShaderEffect::s_sunDir.y = CShaderEffect::s_sunDir.y * scale;
        CShaderEffect::s_sunDir.z = scale * CShaderEffect::s_sunDir.z;
    }

    CShaderEffect::s_sunAmbient = lighting->m_sunAmbient;

    float r = lighting->m_sunDiffuse.x;
    float g = lighting->m_sunDiffuse.y;
    float b = lighting->m_sunDiffuse.z;
    CShaderEffect::s_sunDiffuse.x = r >= 1.0f ? 1.0f : r;
    CShaderEffect::s_sunDiffuse.y = g >= 1.0f ? 1.0f : g;
    CShaderEffect::s_sunDiffuse.z = b < 1.0f ? b : 1.0f;

    C4Vector sunDiffuse;
    sunDiffuse.x = CShaderEffect::s_sunDiffuse.x;
    sunDiffuse.y = CShaderEffect::s_sunDiffuse.y;
    sunDiffuse.z = CShaderEffect::s_sunDiffuse.z;
    sunDiffuse.w = CShaderEffect::s_sunAmbient.x;

    C4Vector sunAmbient;
    sunAmbient.x = CShaderEffect::s_sunAmbient.x;
    sunAmbient.y = CShaderEffect::s_sunAmbient.y;
    sunAmbient.z = CShaderEffect::s_sunAmbient.z;
    sunAmbient.w = CShaderEffect::s_sunDir.x;

    C4Vector sunDir;
    sunDir.x = CShaderEffect::s_sunDir.x;
    sunDir.y = CShaderEffect::s_sunDir.y;
    sunDir.z = CShaderEffect::s_sunDir.z;
    sunDir.w = CShaderEffect::s_sunSpec.x;

    GxShaderConstantsSet(GxSh_Vertex, 10, &sunDiffuse.x, 1);
    GxShaderConstantsSet(GxSh_Vertex, 11, &sunAmbient.x, 1);
    GxShaderConstantsSet(GxSh_Vertex, 12, &sunDir.x, 1);

    if (CShaderEffect::s_localLightCount) {
        CShaderEffect::ComputeLocalLights(&CShaderEffect::s_localLights, CShaderEffect::s_localLightCount, lighting->m_lights, origin);
        GxShaderConstantsSet(GxSh_Vertex, 17, reinterpret_cast<const float*>(&CShaderEffect::s_localLights), sizeof(LocalLights) / sizeof(C4Vector));
    }

#if defined(_WIN32)
    CShadowCache::SetShadowMapGenericInterior(lighting->m_flags & 8);
#endif
}
