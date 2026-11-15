// esp.cpp —— 2D 透视绘制（框 / 骨骼 / 血条 / 距离 / 名字 / 准星 / FOV）
#include "esp.h"
#include "config.h"
#include "render.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace Esp {

static inline ImU32 C(const float col[4], float a = 1.f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(col[0], col[1], col[2], col[3] * a));
}

// 把实体世界坐标投到屏幕；返回是否可见
static void ProjectEntity(const ViewInfo& v, Entity& e) {
    const int sw = v.viewW, sh = v.viewH;
    e.onScreen = false;

    // ---- v4：JS 端在游戏里已经做过 worldToScreen，屏幕坐标是 0..1 归一化比例，
    //      直接乘视口尺寸即可。不再走矩阵 —— 场景遍历拿到的矩阵和真实渲染矩阵不匹配。
    if (e.screenSpace && sw > 0 && sh > 0) {
        e.sHead.x  = e.screenHeadX  * sw;  e.sHead.y  = e.screenHeadY  * sh;
        e.sChest.x = e.screenChestX * sw;  e.sChest.y = e.screenChestY * sh;
        e.sFeet.x  = e.screenFeetX  * sw;  e.sFeet.y  = e.screenFeetY  * sh;
        e.sPelvis  = e.sChest;
        const float hs = e.sFeet.y - e.sHead.y;
        e.onScreen = (hs > 4.f && hs < sh * 1.5f);
        return;
    }

    if (!W2S(v.viewProj, e.head, e.sHead, sw, sh)) return;
    if (!W2S(v.viewProj, e.feet, e.sFeet, sw, sh)) return;
    if (W2S(v.viewProj, e.chest, e.sChest, sw, sh) == false) e.sChest = e.sHead;
    if (W2S(v.viewProj, e.pelvis, e.sPelvis, sw, sh) == false) e.sPelvis = e.sChest;
    if (e.hasBones) {
        for (int i = 0; i < BoneIdx::Count; ++i)
            if (!W2S(v.viewProj, e.bones[i], e.sBones[i], sw, sh)) { e.hasBones = false; break; }
    }
    // 过滤异常尺寸（模型很远时框会退化成点）
    const float h = e.sFeet.y - e.sHead.y;
    e.onScreen = (h > 4.f && h < sh * 1.5f);
}

void Draw(ImDrawList* dl, const ViewInfo& v, std::vector<Entity>& ents) {
    if (!v.valid || !g_cfg.espEnabled) return;
    const float t = g_cfg.boxThickness * Render::UiScale();
    const int sw = v.viewW, sh = v.viewH;

    // 准星
    if (g_cfg.drawCrosshair) {
        const float cx = sw * 0.5f, cy = sh * 0.5f, s = 9.f * Render::UiScale();
        ImU32 c = C(g_cfg.colVisible, 0.85f);
        dl->AddLine(ImVec2(cx - s, cy), ImVec2(cx - 3, cy), c, 1.4f);
        dl->AddLine(ImVec2(cx + 3, cy), ImVec2(cx + s, cy), c, 1.4f);
        dl->AddLine(ImVec2(cx, cy - s), ImVec2(cx, cy - 3), c, 1.4f);
        dl->AddLine(ImVec2(cx, cy + 3), ImVec2(cx, cy + s), c, 1.4f);
    }

    // FOV 圈（自瞄范围可视化）
    if (g_cfg.showFov && g_cfg.aimEnabled) {
        dl->AddCircle(ImVec2(sw * 0.5f, sh * 0.5f), g_cfg.aimFov * Render::UiScale(),
                      C(g_cfg.colFov, 1.f), 64, 1.2f);
    }

    for (auto& e : ents) {
        // ---- v5：JS 透传的 camp/aiLevel/liveState 过滤（未知 = -1 时不过滤，兼容旧写入端）
        if (g_cfg.hideAI && e.isAI) continue;
        if (g_cfg.hideKnocked && e.isKnocked) continue;
        if (g_cfg.onlyEnemy && e.isTeam) continue;
        ProjectEntity(v, e);
        if (!e.onScreen) continue;
        if (e.distance > g_cfg.maxDistance && g_cfg.maxDistance > 0) continue;

        const float boxH = e.sFeet.y - e.sHead.y;
        const float boxW = boxH * 0.52f;                 // 人体宽高比
        const ImVec2 tl(e.sHead.x - boxW * 0.5f, e.sHead.y);
        const ImVec2 br(e.sHead.x + boxW * 0.5f, e.sFeet.y);

        ImU32 col;
        if (e.isTeam)          col = C(g_cfg.colTeam);
        else if (e.visible)    col = C(g_cfg.colVisible);
        else                   col = C(g_cfg.colOccluded);

        // ---- 方框（四角 + 细边，避免遮挡视野）
        if (g_cfg.drawBox) {
            dl->AddRect(tl, br, col, 0.f, 0, t * 0.45f);
            const float corner = std::min(boxW, boxH) * 0.28f;
            // 左上
            dl->AddLine(ImVec2(tl.x, tl.y), ImVec2(tl.x + corner, tl.y), col, t);
            dl->AddLine(ImVec2(tl.x, tl.y), ImVec2(tl.x, tl.y + corner), col, t);
            // 右上
            dl->AddLine(ImVec2(br.x - corner, tl.y), ImVec2(br.x, tl.y), col, t);
            dl->AddLine(ImVec2(br.x, tl.y), ImVec2(br.x, tl.y + corner), col, t);
            // 左下
            dl->AddLine(ImVec2(tl.x, br.y - corner), ImVec2(tl.x, br.y), col, t);
            dl->AddLine(ImVec2(tl.x, br.y), ImVec2(tl.x + corner, br.y), col, t);
            // 右下
            dl->AddLine(ImVec2(br.x - corner, br.y), ImVec2(br.x, br.y), col, t);
            dl->AddLine(ImVec2(br.x, br.y - corner), ImVec2(br.x, br.y), col, t);
        }

        // ---- 血条（左侧竖条）
        if (g_cfg.drawHealthBar) {
            const float bw = std::max(2.5f, boxW * 0.10f);
            const float x  = tl.x - bw - 3.f * Render::UiScale();
            float ratio = (e.maxHp > 0.f) ? ClampF(e.hp / e.maxHp, 0.f, 1.f) : 1.f;
            dl->AddRectFilled(ImVec2(x, tl.y), ImVec2(x + bw, br.y), IM_COL32(0, 0, 0, 150));
            dl->AddRectFilled(ImVec2(x, br.y - (br.y - tl.y) * ratio), ImVec2(x + bw, br.y),
                              C(g_cfg.colHealth));
            dl->AddRect(ImVec2(x, tl.y), ImVec2(x + bw, br.y), IM_COL32(0, 0, 0, 200));
        }

        // ---- 骨骼
        if (g_cfg.drawSkeleton && e.hasBones) {
            auto L = [&](int a, int b) {
                dl->AddLine(ImVec2(e.sBones[a].x, e.sBones[a].y),
                            ImVec2(e.sBones[b].x, e.sBones[b].y),
                            C(g_cfg.colSkeleton), g_cfg.skeletonThick * Render::UiScale());
            };
            const int HD = BoneIdx::Head, NK = BoneIdx::Neck, CST = BoneIdx::Chest,
                      SPN = BoneIdx::Spine, PLV = BoneIdx::Pelvis,
                      LS = BoneIdx::LShoulder, RS = BoneIdx::RShoulder,
                      LE = BoneIdx::LElbow, RE = BoneIdx::RElbow,
                      LH = BoneIdx::LHip, RH = BoneIdx::RHip,
                      LK = BoneIdx::LKnee, RK = BoneIdx::RKnee,
                      LF = BoneIdx::LFoot, RF = BoneIdx::RFoot;
            L(HD, NK); L(NK, CST); L(CST, SPN); L(SPN, PLV);
            L(NK, LS); L(LS, LE);
            L(NK, RS); L(RS, RE);
            L(PLV, LH); L(LH, LK); L(LK, LF);
            L(PLV, RH); L(RH, RK); L(RK, RF);
        }

        // ---- 距离 + 名字（v5：showTag 时附上数据源判定，便于核对 camp/aiLevel/liveState）
        char buf[96];
        const float fs = 15.f * Render::UiScale();
        if (g_cfg.drawDistance) {
            if (g_cfg.showTag)
                snprintf(buf, sizeof(buf), "%d m c%d a%d l%d", (int)(e.distance * g_cfg.worldScale),
                         e.camp, e.aiLevel, e.liveState);
            else
                snprintf(buf, sizeof(buf), "%d m", (int)(e.distance * g_cfg.worldScale));
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, buf);
            ImVec2 p(br.x + 4.f * Render::UiScale(), e.sChest.y - ts.y * 0.5f);
            dl->AddText(ImGui::GetFont(), fs, p, C(g_cfg.colText), buf);
        }
        if (g_cfg.drawName && e.name[0]) {
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, e.name);
            ImVec2 p(e.sHead.x - ts.x * 0.5f, tl.y - ts.y - 2.f * Render::UiScale());
            dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 190), e.name);
            dl->AddText(ImGui::GetFont(), fs, p, C(g_cfg.colText), e.name);
        }

        // ---- 瞄准射线
        if (g_cfg.drawAimLine) {
            dl->AddLine(ImVec2(sw * 0.5f, sh * 0.5f), ImVec2(e.sChest.x, e.sChest.y),
                        C(g_cfg.colVisible, 0.35f), 1.2f * Render::UiScale());
        }

        // ---- v5：JS 解算的提前量落点（flags bit4）。JS 已把预瞄点转成归一化屏幕坐标，
        //      C++ 侧不再做任何弹道计算，只做一次视口换算 + 画点。
        if (g_cfg.drawLeadPoint && e.hasLead) {
            const float lx = e.leadScrX * sw, ly = e.leadScrY * sh;
            if (lx > -sw && lx < sw * 2.f && ly > -sh && ly < sh * 2.f) {
                const float r  = 4.0f * Render::UiScale();
                const float th = 1.6f * Render::UiScale();
                const ImU32 lc = C(g_cfg.colLead);
                dl->AddCircle(ImVec2(lx, ly), r + 3.f * Render::UiScale(), lc, 0, th * 0.8f);
                dl->AddLine(ImVec2(lx - r, ly), ImVec2(lx + r, ly), lc, th);
                dl->AddLine(ImVec2(lx, ly - r), ImVec2(lx, ly + r), lc, th);
                // 当前胸口 → 预瞄点 的连线，便于直观看到提前量方向
                if (g_cfg.drawAimLine)
                    dl->AddLine(ImVec2(e.sChest.x, e.sChest.y), ImVec2(lx, ly),
                                C(g_cfg.colLead, 0.45f), 1.0f * Render::UiScale());
                if (g_cfg.showTag) {
                    char lb[64];
                    snprintf(lb, sizeof(lb), "%.0fm %.2fs", e.leadMeters, e.flightTime);
                    dl->AddText(ImGui::GetFont(), 12.f * Render::UiScale(),
                                ImVec2(lx + r + 4.f * Render::UiScale(), ly - 6.f * Render::UiScale()),
                                lc, lb);
                }
            }
        }
    }
}

void DrawWatermark(ImDrawList* dl, float w, float h, int entCount, const char* sensor, float ms) {
    if (!g_cfg.debugOverlay) return;
    char buf[192];
    snprintf(buf, sizeof(buf), "实体 %d | %s | %.2f ms | %dx%d | 字体:%s",
             entCount, sensor ? sensor : "-", ms, (int)w, (int)h,
             Render::FontHasCjk() ? "CJK" : "无中文");
    const float s = Render::UiScale();
    dl->AddText(ImGui::GetFont(), 16.f * s,
                ImVec2(10.f * s, 8.f * s), IM_COL32(120, 255, 160, 220), buf);
}

}  // namespace Esp
