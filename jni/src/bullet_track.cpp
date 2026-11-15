// bullet_track.cpp —— 子弹追踪：弹道解算 + 预测点/弧线可视化（不产生任何输入事件）
#include "bullet_track.h"
#include "config.h"
#include "render.h"
#include "imgui.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <chrono>

namespace Track {

static Hit   g_hit;
static int   g_cand = 0;
static float g_ms   = 0.f;
static bool  g_useMatrix = true;   // v4：本次解算是否走矩阵（false=JS 已给屏幕坐标，弧线不画）

static inline float Dist3(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

void Reset() { g_hit = Hit(); g_cand = 0; g_ms = 0.f; }

// 弹道解算：以固定点迭代收敛飞行时间，同时算提前量与下坠
static void Solve(const Vec3& cam, const Vec3& target, const Vec3& vel, Hit& out) {
    const float v = std::max(50.f, g_cfg.btBulletSpeed);
    const float g = g_cfg.btUseGravity ? g_cfg.btGravity : 0.f;

    float t = Dist3(cam, target) / v;
    if (t > 3.0f) t = 3.0f;
    for (int it = 0; it < 4; ++it) {
        const Vec3 p(target.x + vel.x * t,
                     target.y + vel.y * t - 0.5f * g * t * t,
                     target.z + vel.z * t);
        const float d = Dist3(cam, p);
        float nt = d / v;
        if (g_cfg.btDragModel) nt *= (1.f + g_cfg.btDragK * d * d);
        if (nt > 3.0f) nt = 3.0f;
        if (fabsf(nt - t) < 1e-4f) { t = nt; break; }
        t = nt;
    }

    out.leadVec    = Vec3(vel.x * t, vel.y * t, vel.z * t);
    out.leadMeters = sqrtf(out.leadVec.x * out.leadVec.x +
                           out.leadVec.y * out.leadVec.y +
                           out.leadVec.z * out.leadVec.z);
    out.dropMeters = 0.5f * g * t * t;
    out.point      = Vec3(target.x + out.leadVec.x,
                          target.y + out.leadVec.y - out.dropMeters,
                          target.z + out.leadVec.z);
    out.dist       = Dist3(cam, out.point);
    out.flightTime = t;
}

void Update(const ViewInfo& v, std::vector<Entity>& ents, float dt) {
    (void)dt;
    const auto t0 = std::chrono::steady_clock::now();
    g_cand = 0;
    if (!g_cfg.btEnabled || !v.valid) { g_hit.valid = false; return; }

    const int sw = v.viewW, sh = v.viewH;
    if (sw <= 0 || sh <= 0) { g_hit.valid = false; return; }

    const float cx   = sw * 0.5f, cy = sh * 0.5f;
    const float fovR = g_cfg.btFovPx * Render::UiScale();

    Hit   best;
    float bestD = 1e9f;

    for (size_t i = 0; i < ents.size(); ++i) {
        Entity& e = ents[i];
        if (e.isDead) continue;
        if (e.distance > g_cfg.btMaxDist) continue;

        Vec3 tgt = (g_cfg.btBone == 1) ? e.chest : e.head;
        if (!e.screenSpace) {
            if (tgt.x == 0.f && tgt.y == 0.f && tgt.z == 0.f) tgt = e.chest;
            if (tgt.x == 0.f && tgt.y == 0.f && tgt.z == 0.f) continue;
        }

        Vec2 now2;
        bool haveNow = false;
        if (e.screenSpace) {
            // v4：屏幕坐标由 JS 侧归一化给出，不需要矩阵
            now2.x = ((g_cfg.btBone == 1) ? e.screenChestX : e.screenHeadX) * sw;
            now2.y = ((g_cfg.btBone == 1) ? e.screenChestY : e.screenHeadY) * sh;
            haveNow = true;
        } else if (W2S(v.viewProj, tgt, now2, sw, sh)) {
            haveNow = true;
        }
        if (!haveNow) continue;

        const float dx = now2.x - cx, dy = now2.y - cy;
        const float sd = sqrtf(dx * dx + dy * dy);
        if (g_cfg.btOnlyInFov && sd > fovR) continue;
        ++g_cand;
        if (sd >= bestD) continue;

        Hit h;
        h.entIndex = (int)i;
        h.sNow     = now2;
        if (e.screenSpace && e.hasLead) {
            // JS 已解算提前量/下坠/命中点屏幕坐标 → 直接采用，零矩阵依赖
            h.leadMeters = e.leadMeters;
            h.dropMeters = e.dropMeters;
            h.flightTime = e.flightTime;
            h.dist       = e.distance;
            h.point      = tgt;
            h.sPoint.x   = e.leadScrX * sw;
            h.sPoint.y   = e.leadScrY * sh;
            h.onScreen   = (e.leadScrX > 0.f || e.leadScrY > 0.f);
            h.inFov      = true;
            h.valid      = true;
            g_useMatrix  = false;
        } else {
            Solve(v.camPos, tgt, e.velocity, h);
            h.onScreen = W2S(v.viewProj, h.point, h.sPoint, sw, sh);
            h.inFov    = true;
            h.valid    = true;
            g_useMatrix = true;
        }

        best  = h;
        bestD = sd;
    }

    g_hit = best;
    if (!best.valid) g_hit.valid = false;
    g_ms = std::chrono::duration<float, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

void Draw(ImDrawList* dl, const ViewInfo& v) {
    if (!dl || !g_cfg.btEnabled || !g_hit.valid || !g_hit.onScreen || !v.valid) return;

    const float s     = Render::UiScale();
    const ImU32 colP  = IM_COL32(255, 90, 90, 235);
    const ImU32 colLn = IM_COL32(255, 90, 90, 140);
    const ImU32 colArc= IM_COL32(110, 205, 255, 110);
    const ImU32 colTxt= IM_COL32(255, 238, 140, 240);

    const ImVec2 pNow(g_hit.sNow.x, g_hit.sNow.y);
    const ImVec2 pHit(g_hit.sPoint.x, g_hit.sPoint.y);

    // 当前点 → 预测点（提前量）
    dl->AddLine(pNow, pHit, colLn, 1.2f * s);

    if (g_cfg.btShowPoint) {
        const float r = 8.f * s;
        dl->AddCircle(pHit, r, colP, 20, 1.7f * s);
        dl->AddCircle(pHit, r * 0.42f, colP, 12, 1.2f * s);
        dl->AddLine(ImVec2(pHit.x - r * 1.5f, pHit.y), ImVec2(pHit.x - r * 0.6f, pHit.y), colP, 1.2f * s);
        dl->AddLine(ImVec2(pHit.x + r * 0.6f, pHit.y), ImVec2(pHit.x + r * 1.5f, pHit.y), colP, 1.2f * s);
        dl->AddLine(ImVec2(pHit.x, pHit.y - r * 1.5f), ImVec2(pHit.x, pHit.y - r * 0.6f), colP, 1.2f * s);
        dl->AddLine(ImVec2(pHit.x, pHit.y + r * 0.6f), ImVec2(pHit.x, pHit.y + r * 1.5f), colP, 1.2f * s);
    }

    // 弹道弧：从枪口（≈相机）到命中点，按飞行时间采样物理抛物线后投影
    // v4：JS 已给屏幕坐标时没有可靠矩阵，弧线跳过（点/信息仍然画）
    if (g_cfg.btShowArc && g_hit.flightTime > 0.001f && g_useMatrix) {
        const Vec3 A = v.camPos;
        const Vec3 Bn(g_hit.point.x, g_hit.point.y + g_hit.dropMeters, g_hit.point.z);
        const float g = g_cfg.btUseGravity ? g_cfg.btGravity : 0.f;
        const int N = (int)ClampF(g_cfg.btArcSegments, 4.f, 48.f);

        ImVec2 prev;
        bool hasPrev = false;
        for (int i = 0; i <= N; ++i) {
            const float u  = (float)i / (float)N;
            const float tt = g_hit.flightTime * u;
            const Vec3 p(A.x + (Bn.x - A.x) * u,
                         A.y + (Bn.y - A.y) * u - 0.5f * g * tt * tt,
                         A.z + (Bn.z - A.z) * u);
            Vec2 sp;
            if (W2S(v.viewProj, p, sp, v.viewW, v.viewH)) {
                if (hasPrev) dl->AddLine(prev, ImVec2(sp.x, sp.y), colArc, 1.1f * s);
                prev = ImVec2(sp.x, sp.y);
                hasPrev = true;
            } else {
                hasPrev = false;
            }
        }
    }

    if (g_cfg.btShowInfo) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%.0fm  T=%.2fs  lead %.1fm  drop %.1fm",
                 g_hit.dist * g_cfg.worldScale, g_hit.flightTime,
                 g_hit.leadMeters, g_hit.dropMeters);
        const float fs = 14.f * s;
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, buf);
        const ImVec2 tp(pHit.x - ts.x * 0.5f, pHit.y + 14.f * s);
        dl->AddText(ImGui::GetFont(), fs, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), buf);
        dl->AddText(ImGui::GetFont(), fs, tp, colTxt, buf);
    }
}

const Hit& Current()      { return g_hit; }
int        CandidateCount() { return g_cand; }
float      LastSolveMs()  { return g_ms; }

}  // namespace Track
