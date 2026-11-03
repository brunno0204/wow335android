#include "Engine/Source/Model2/M2Light.h"
// Lighting state is independent of model animation/skin loading. Keep its
// original definitions in the Model2 module and reuse them in the native port.

// VERIFY: 4f897dd8 manual
// OFFSET: 0x008248C0
CM2Lighting::CM2Lighting() {
#if !defined(_WIN32)
    this->Initialize(nullptr, CAaSphere{});
#endif
    this->m_sphere4.c.x = 0.0f;
    this->m_sphere4.r = 0.0f;
    this->m_sphere4.c.y = 0.0f;
    this->m_sphere4.c.z = 0.0f;
    this->m_vector18 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector24 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector30 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector3C = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector48 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunAmbient = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunDiffuse = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunSpecular = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunDir = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_fogColor = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_liquidPlane.n.x = 0.0f;
    this->m_liquidPlane.n.y = 0.0f;
    this->m_liquidPlane.n.z = 1.0f;
    this->m_liquidPlane.d = 0.0f;
}

// VERIFY: 3fa6010f auto
// OFFSET: 0x004E2730
void CM2Lighting::Reset() {
    this->m_sunAmbient = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunDiffuse = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunSpecular = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunDir = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector18 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector24 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector30 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector3C = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector48 = C3Vector(0.0f, 0.0f, 0.0f);
}

// VERIFY: bf5b6c6a manual
// OFFSET: 0x00790440
CM2Lighting::CM2Lighting(const CAaSphere& sphere) {
    this->m_sphere4.c.x = 0.0f;
    this->m_sphere4.r = 0.0f;
    this->m_sphere4.c.y = 0.0f;
    this->m_sphere4.c.z = 0.0f;
    this->m_vector18 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector24 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector30 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector3C = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector48 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunAmbient = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunDiffuse = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunSpecular = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunDir = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_fogColor = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_liquidPlane.n.x = 0.0f;
    this->m_liquidPlane.n.y = 0.0f;
    this->m_liquidPlane.n.z = 1.0f;
    this->m_liquidPlane.d = 0.0f;
    this->Initialize(nullptr, sphere);
}

// VERIFY: 8535c10a manual
// OFFSET: 0x007B0F10
CM2Lighting::CM2Lighting(const C3Vector& position) {
    this->m_sphere4.c.x = 0.0f;
    this->m_sphere4.r = 0.0f;
    this->m_sphere4.c.y = 0.0f;
    this->m_sphere4.c.z = 0.0f;
    this->m_vector18 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector24 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector30 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector3C = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_vector48 = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunAmbient = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunDiffuse = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunSpecular = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_sunDir = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_fogColor = C3Vector(0.0f, 0.0f, 0.0f);
    this->m_liquidPlane.n.x = 0.0f;
    this->m_liquidPlane.n.y = 0.0f;
    this->m_liquidPlane.n.z = 1.0f;
    this->m_liquidPlane.d = 0.0f;

    CAaSphere sphere;
    sphere.c = position;
    sphere.r = 0.0f;
    this->Initialize(nullptr, sphere);
}
