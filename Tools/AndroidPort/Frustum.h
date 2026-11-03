#pragma once
#include "Engine/Source/Tempest/C44Matrix.h"
#include <cmath>
namespace wow::android {
// View frustum of a row-vector view*projection matrix (clip = [x y z 1] * M), for sphere culling.
struct Frustum {
    float planes[6][4]{};
    explicit Frustum(const C44Matrix &m) {
        const float c[4][4]{{m.a0, m.b0, m.c0, m.d0}, {m.a1, m.b1, m.c1, m.d1}, {m.a2, m.b2, m.c2, m.d2},
                            {m.a3, m.b3, m.c3, m.d3}}; // columns: x, y, z, w
        for (int i = 0; i < 4; ++i) {
            planes[0][i] = c[3][i] + c[0][i]; // left
            planes[1][i] = c[3][i] - c[0][i]; // right
            planes[2][i] = c[3][i] + c[1][i]; // bottom
            planes[3][i] = c[3][i] - c[1][i]; // top
            planes[4][i] = c[3][i] + c[2][i]; // near
            planes[5][i] = c[3][i] - c[2][i]; // far
        }
        for (auto &p : planes) {
            const float n = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (n > 0)
                for (float &v : p)
                    v /= n;
        }
    }
    bool sphere(float x, float y, float z, float radius) const {
        for (const auto &p : planes)
            if (p[0] * x + p[1] * y + p[2] * z + p[3] < -radius)
                return false;
        return true;
    }
};
} // namespace wow::android
