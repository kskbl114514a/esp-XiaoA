// aimbot.cpp —— 目标选择 + 平滑转向（通过触摸注入落地）
#include "aimbot.h"
#include "config.h"
#include "log.h"
#include "touch.h"
#include "evtouch.h"
#include "java_bridge.h"

#include <cmath>
#include <cstring>

namespace Aim {

static uint64_t g_targetId = 0;
static double   g_lockAt = 0;        // 首次锁定时间
static double   g_lastSwitch = 0;    // 上次换目标时间
static double   g_now = 0;
static float    g_lastDelta = 0;

static double NowSec() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// g_picked 指向上一帧 ents 里的元素：ents 由 Sensor 每帧重建，跨帧后该指针随时悬垂。
// 因此对外只承诺「Aim::Update 返回后、下一帧 Sensor 刷新前有效」，跨帧请用 id 比对。
static Entity* g_picked = nullptr;

Entity* CurrentTarget() { return g_picked; }
bool HasTarget() { return g_picked != nullptr && g_targetId != 0; }
float LastDeltaPx() { return g_lastDelta; }

void Reset() {
    g_targetId = 0;
    g_picked = nullptr;
    g_lockAt = 0;
    g_lastSwitch = 0;
    g_lastDelta = 0;
    JavaBridge::EndDrag();
}

// 目标瞄准点（世界坐标）
//   0=头  1=胸  2=最近骨骼（在头/胸/盆/脚四点里挑屏幕上最靠准星的那个）
static Vec3 AimPoint(const Entity& e, const Mat4& vp, int sw, int sh, float cx, float cy) {
    if (g_cfg.aimBone == 0) return e.head;
    if (g_cfg.aimBone == 1) return e.chest;

    const Vec3 cand[4] = { e.head, e.chest, e.pelvis, e.feet };
    Vec3 best = e.chest;
    float bd = 1e18f;
    for (int i = 0; i < 4; ++i) {
        Vec2 sp;
        if (!W2S(vp, cand[i], sp, sw, sh)) continue;
        float d = (sp.x - cx) * (sp.x - cx) + (sp.y - cy) * (sp.y - cy);
        if (d < bd) { bd = d; best = cand[i]; }
    }
    return best;
}

static float PxPerDeg(float sh, float fovY) {
    // fovY 是**垂直**视场角，配的必须是屏幕半高，不是半宽。
    // 原实现传 sw（宽）进来除以 tan(fovY/2)，16:9 竖屏/横屏下每度像素直接偏 1.78 倍，
    // aimSpeed 的手感随分辨率漂移。
    float half = fovY * 0.5f * 3.14159265f / 180.f;
    if (half < 0.05f) half = 0.05f;
    if (half > 1.52f) half = 1.52f;                  // fovY 上限 174°，防止 tan 爆掉
    return (sh * 0.5f) / tanf(half);
}

void Update(const ViewInfo& v, std::vector<Entity>& ents, float dt) {
    g_now = NowSec();
    if (!v.valid) return;
    if (!g_cfg.aimEnabled) { if (JavaBridge::Dragging()) JavaBridge::EndDrag(); g_picked = nullptr; return; }

    // 开火判定：Cocos 触摸 hook 全失败时（houdini 环境常见），改由 evdev 原始输入判定
    bool fireHeld = TouchHook::IsFireHeld() || EvdevTouch::FireHeld();
    if (g_cfg.aimOnFireOnly && !fireHeld) {
        if (JavaBridge::Dragging()) JavaBridge::EndDrag();
        g_picked = nullptr;
        g_targetId = 0;
        return;
    }
    // 菜单展开时不打（自己都在点菜单）
    if (g_cfg.menuOpen) {
        if (JavaBridge::Dragging()) JavaBridge::EndDrag();
        g_picked = nullptr;
        g_targetId = 0;
        return;
    }

    const float sw = (float)v.viewW, sh = (float)v.viewH;
    if (sw < 16.f || sh < 16.f) return;
    if (!v.hasProj) return;                          // viewProj 全 0 时 W2S 只会吐出垃圾坐标
    if (!(dt > 0.f)) dt = 0.f;
    if (dt > 0.05f) dt = 0.05f;                      // 掉帧后 dt 可能是半秒，一步转过去就是"瞬移"特征
    const float cx = sw * 0.5f, cy = sh * 0.5f;
    const float fovPx = g_cfg.aimFov * (sw / 1080.0f);

    // ---------- 1) 选目标
    Entity* best = nullptr;
    float   bestScore = 1e9f;
    float   bestPx = 0, bestPy = 0;

    for (auto& e : ents) {
        if (e.isDead) continue;
        if (g_cfg.aimIgnoreTeam && e.isTeam) continue;
        if (g_cfg.aimVisibleOnly && !e.visible) continue;
        float distM = e.distance * (g_cfg.worldScale > 0.01f ? g_cfg.worldScale : 1.f);
        if (g_cfg.aimMaxDist > 0 && distM > g_cfg.aimMaxDist) continue;

        Vec3 p = AimPoint(e, v.viewProj, (int)sw, (int)sh, cx, cy);
        // 预判：按弹速提前量（与 BulletTrack::Solve 同口径：飞行时间夹在 0..3s，并补重力下坠量）
        if (g_cfg.aimPredict && g_cfg.bulletSpeed > 1.f) {
            float t = ClampF(distM / g_cfg.bulletSpeed, 0.f, 3.f);
            p.x += e.velocity.x * t;
            p.z += e.velocity.z * t;
            p.y += e.velocity.y * t + 0.5f * g_cfg.btGravity * t * t;   // 下坠补偿：瞄点抬高
        }
        Vec2 sp;
        if (!W2S(v.viewProj, p, sp, (int)sw, (int)sh)) continue;
        float d = sqrtf((sp.x - cx) * (sp.x - cx) + (sp.y - cy) * (sp.y - cy));
        if (d > fovPx) continue;
        if (d < bestScore) { bestScore = d; best = &e; bestPx = sp.x; bestPy = sp.y; }
    }

    // ---------- 2) 粘滞：已锁定的目标只要还在 FOV 内就不换（规避"乱切"特征）
    if (g_targetId) {
        for (auto& e : ents) {
            if (e.id != g_targetId || e.isDead) continue;
            if (g_cfg.aimVisibleOnly && !e.visible) break;
            if (g_cfg.aimIgnoreTeam && e.isTeam) break;
            Vec3 p = AimPoint(e, v.viewProj, (int)sw, (int)sh, cx, cy);
            if (g_cfg.aimPredict && g_cfg.bulletSpeed > 1.f) {
                float t = ClampF(e.distance * (g_cfg.worldScale > 0.01f ? g_cfg.worldScale : 1.f)
                                 / g_cfg.bulletSpeed, 0.f, 3.f);
                p.x += e.velocity.x * t; p.z += e.velocity.z * t;
                p.y += e.velocity.y * t + 0.5f * g_cfg.btGravity * t * t;
            }
            Vec2 sp;
            if (!W2S(v.viewProj, p, sp, (int)sw, (int)sh)) break;
            float d = sqrtf((sp.x - cx) * (sp.x - cx) + (sp.y - cy) * (sp.y - cy));
            if (d <= fovPx * 1.5f && (g_now - g_lastSwitch) * 1000.0 > g_cfg.aimSwitchDelayMs) {
                best = &e; bestPx = sp.x; bestPy = sp.y;
            }
            break;
        }
    }

    if (!best) {
        if (JavaBridge::Dragging()) JavaBridge::EndDrag();
        g_picked = nullptr;
        g_targetId = 0;
        return;
    }

    if (best->id != g_targetId) {
        g_targetId = best->id;
        g_lastSwitch = g_now;
        g_lockAt = 0;
    }
    g_picked = best;

    // 首锁延迟
    if (g_lockAt == 0) g_lockAt = g_now;
    if ((g_now - g_lockAt) * 1000.0 < g_cfg.aimDelayMs) return;

    // ---------- 3) 转向
    float dx = bestPx - cx;
    float dy = bestPy - cy;
    float len = sqrtf(dx * dx + dy * dy);
    g_lastDelta = len;
    if (len < 1.5f) { if (JavaBridge::Dragging()) JavaBridge::EndDrag(); return; }

    // 速度：aimSpeed 视为「每秒可转的度数」，换算成像素/秒
    float pxPerDeg = PxPerDeg(sh, v.fovY > 1.f ? v.fovY : 60.f);
    float maxPxPerSec = pxPerDeg * g_cfg.aimSpeed;
    float step = maxPxPerSec * dt;
    if (step > len) step = len;

    // 二次平滑：越接近目标越慢，避免抖动与"锁死"手感
    float smoothK = 1.0f - ClampF(g_cfg.aimSmooth, 0.f, 0.95f) * 0.7f;
    float distK = ClampF(len / (fovPx + 1.f), 0.35f, 1.0f);
    step *= smoothK * distK;
    if (step < 0.6f) step = 0.6f;

    float mx = dx / len * step;
    float my = dy / len * step;
    bool ok = JavaBridge::Nudge(mx, my);

    if (g_cfg.aimDebug) {
        static double lastLog = 0;
        if (g_now - lastLog > 0.5) {
            lastLog = g_now;
            LOGI("Aim: 锁定 id=%llu 屏距=%.1fpx 步长=%.2fpx 结果=%d 错误=%s",
                 (unsigned long long)best->id, len, step, (int)ok, JavaBridge::LastError());
        }
    }
}

}  // namespace Aim
