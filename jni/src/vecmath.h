// math.h —— ESP / 自瞄用到的全部线性代数与投影
// 矩阵约定：Cocos Creator (3.x) 与 GL 一致，列主序 column-major，m[12..14] 为平移。
#pragma once
#include <cmath>
#include <algorithm>

struct Vec2 { float x, y; };
struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    float Length() const { return sqrtf(x * x + y * y + z * z); }
    float Length2D() const { return sqrtf(x * x + z * z); }
};

struct Mat4 {
    float m[16];
    static Mat4 Identity() { Mat4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f; return r; }
};

// ---------------------------------------------------------------- W2S
// 列主序：clip.x = m[0]*x + m[4]*y + m[8]*z + m[12]
// 若发现画面左右/上下镜像整体漂移，说明矩阵是行主序，调用 W2S_RowMajor。
inline bool W2S(const Mat4& vp, const Vec3& world, Vec2& out, int screenW, int screenH) {
    const float cx = vp.m[0] * world.x + vp.m[4] * world.y + vp.m[8] * world.z + vp.m[12];
    const float cy = vp.m[1] * world.x + vp.m[5] * world.y + vp.m[9] * world.z + vp.m[13];
    const float cz = vp.m[2] * world.x + vp.m[6] * world.y + vp.m[10] * world.z + vp.m[14];
    const float cw = vp.m[3] * world.x + vp.m[7] * world.y + vp.m[11] * world.z + vp.m[15];
    if (cw < 0.001f) return false;                    // 在相机背后
    const float ndcX = cx / cw;
    const float ndcY = cy / cw;
    if (ndcX < -3.f || ndcX > 3.f || ndcY < -3.f || ndcY > 3.f) return false;
    out.x = (screenW * 0.5f) * (ndcX + 1.f);
    out.y = (screenH * 0.5f) * (1.f - ndcY);          // NDC 的 Y 向上，屏幕 Y 向下
    return true;
}

inline bool W2S_RowMajor(const Mat4& vp, const Vec3& w, Vec2& out, int sw, int sh) {
    Mat4 t{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t.m[c * 4 + r] = vp.m[r * 4 + c];
    return W2S(t, w, out, sw, sh);
}

// ---------------------------------------------------------------- 平滑转向
// 最短路径插值：把角度差归一到 [-180,180]，按 maxDelta 限速。
inline float StepAngle(float cur, float target, float maxDelta) {
    float d = target - cur;
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    if (fabsf(d) <= maxDelta) return target;
    return cur + (d > 0 ? maxDelta : -maxDelta);
}

inline float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float LerpF(float a, float b, float t) { return a + (b - a) * t; }
