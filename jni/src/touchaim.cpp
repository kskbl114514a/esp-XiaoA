// touchaim.cpp —— 触摸自瞄
// ---------------------------------------------------------------------------
// 投递通道：/dev/uinput 上的一根独立 slot 虚拟手指（evtouch 里实现）。
//   - 玩家真实手指走 evdev 物理设备 / 穿透重放（slot 0..MAX_SLOTS-2）
//   - 自瞄手指走 AIM_SLOT，内核看是另一根手指 → 不与玩家抢 pointerId=0
//   - uinput 不可用（没权限/设备没起来）时自动回落 JavaBridge::Nudge 的 MotionEvent 注入
//
// 控制逻辑：闭环比例拖动
//   1) 选目标（FOV 内、距离、可见、队伍过滤，带粘滞锁定避免乱切）
//   2) 算屏幕偏差 (dx,dy) = 目标点 - 准星
//   3) 每帧往偏差方向滑一小段（gain 可调、单帧步长限幅），游戏把位移转成视角旋转
//   4) 偏差收敛到死区内就停手（保持按住不动），不抖
//   5) 手指滑出活动半径 → 抬起、下一帧在锚点重按，避免长期按压被识别成长按
// ---------------------------------------------------------------------------
#include "touchaim.h"
#include "config.h"
#include "log.h"
#include "touch.h"
#include "evtouch.h"
#include "java_bridge.h"
#include "vecmath.h"

#include <cmath>
#include <cstdio>
#include <ctime>

namespace TouchAim {

static double NowSec() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

struct State {
    bool     down       = false;
    float    x          = 0, y = 0;    // 虚拟手指当前 surface 坐标
    double   lastSend   = 0;
    double   lastUp     = 0;
    double   lockAt     = 0;
    double   lastSwitch = 0;
    uint64_t targetId   = 0;
    bool     have       = false;
    float    tgtX       = 0, tgtY = 0;
    float    lastDelta  = 0;
    int      moves      = 0;
    int      recenters  = 0;
    bool     uinputOk   = false;
    double   chanProbe  = 0;           // 通道重试节流
    double   chanReady  = 0;           // uinput 设备就绪时刻（等 InputReader 注册）
    char     err[128]   = {0};
};

static State   g;
static Entity* g_picked = nullptr;

static void SetErr(const char* what) {
    snprintf(g.err, sizeof(g.err), "%s", what ? what : "");
}

// 目标瞄准点（世界坐标）：0=头 1=胸 2=最近骨骼
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

static Vec3 WithPredict(const Entity& e, Vec3 p) {
    if (g_cfg.aimPredict && g_cfg.bulletSpeed > 1.f) {
        const float t = e.distance / g_cfg.bulletSpeed;
        p.x += e.velocity.x * t;
        p.y += e.velocity.y * t;
        p.z += e.velocity.z * t;
    }
    return p;
}

static float PxPerDeg(float sw, float fovY) {
    float half = fovY * 0.5f * 3.14159265f / 180.f;
    if (half < 0.05f) half = 0.05f;
    return (sw * 0.5f) / tanf(half);
}

// ---------------------------------------------------------------- 对外状态
Entity* CurrentTarget() { return g_picked; }
bool    HasTarget()     { return g.have && g_picked != nullptr; }
float   LastDeltaPx()   { return g.lastDelta; }
int     MoveFrames()    { return g.moves; }
const char* LastError() { return g.err; }
bool    ChannelOk()     { return g.uinputOk; }

const char* StateText() {
    if (!g_cfg.aimEnabled) return "关";
    if (!g.uinputOk)       return "通道未就绪";
    if (!g.have)           return "无目标";
    if (!g.down)           return "待按下";
    return "拖动中";
}

void Reset() {
    if (g.down) { EvdevTouch::AimTouchUp(); JavaBridge::EndDrag(); }
    g.down = false;
    g.have = false;
    g_picked = nullptr;
    g.targetId = 0;
    g.lockAt = 0;
    g.lastDelta = 0;
    g.moves = 0;
    g.recenters = 0;
}

// ---------------------------------------------------------------- 通道
// uinput 需要 root 侧 chmod 666 /dev/uinput；不可用时回落 Java 注入
static bool EnsureChannel(double now) {
    if (g.uinputOk) return true;
    if (now - g.chanProbe < 3.0) return false;    // 3 秒重试一次，别每帧 open
    g.chanProbe = now;
    if (EvdevTouch::AimTouchReady()) {
        g.uinputOk = true;
        g.chanReady = now;
        LOGI("自瞄通道就绪：uinput 虚拟手指（slot 独立）");
    } else {
        g.uinputOk = false;
        SetErr("uinput 未就绪，回落 MotionEvent 注入（会与真实手指抢 pointerId=0）");
        LOGW("%s", g.err);
    }
    return g.uinputOk;
}

static void ReleaseFinger(const char* why) {
    if (g.down) {
        EvdevTouch::AimTouchUp();
        JavaBridge::EndDrag();
        g.down = false;
        g.lastUp = NowSec();
    }
    if (why && g_cfg.aimDebug) LOGI("自瞄松开：%s", why);
}

// ---------------------------------------------------------------- 主循环
void Update(const ViewInfo& v, std::vector<Entity>& ents, float dt) {
    const double now = NowSec();
    g.lastDelta = 0;

    if (!g_cfg.aimEnabled) { ReleaseFinger("关闭"); g.have = false; g_picked = nullptr; g.targetId = 0; return; }
    if (g_cfg.menuOpen)    { ReleaseFinger("菜单展开"); g.have = false; g_picked = nullptr; g.targetId = 0; return; }
    if (!v.valid)          { ReleaseFinger("数据无效"); g.have = false; g_picked = nullptr; return; }

    // 开火门控
    const bool fireHeld = TouchHook::IsFireHeld() || EvdevTouch::FireHeld();
    if (g_cfg.aimOnFireOnly && !fireHeld) {
        ReleaseFinger("未开火");
        g.have = false;
        g_picked = nullptr; g.targetId = 0;
        return;
    }

    const float sw = (float)v.viewW, sh = (float)v.viewH;
    if (sw < 16.f) return;
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
        const float distM = e.distance * (g_cfg.worldScale > 0.01f ? g_cfg.worldScale : 1.f);
        if (g_cfg.aimMaxDist > 0 && distM > g_cfg.aimMaxDist) continue;

        Vec3 p = WithPredict(e, AimPoint(e, v.viewProj, (int)sw, (int)sh, cx, cy));
        Vec2 sp;
        if (!W2S(v.viewProj, p, sp, (int)sw, (int)sh)) continue;
        const float d = sqrtf((sp.x - cx) * (sp.x - cx) + (sp.y - cy) * (sp.y - cy));
        if (d > fovPx) continue;
        if (d < bestScore) { bestScore = d; best = &e; bestPx = sp.x; bestPy = sp.y; }
    }

    // ---------- 2) 粘滞：锁定中的目标只要还在 1.5 倍 FOV 内就不换（规避乱切特征）
    if (g.targetId) {
        for (auto& e : ents) {
            if (e.id != g.targetId || e.isDead) continue;
            if (g_cfg.aimVisibleOnly && !e.visible) break;
            Vec3 p = WithPredict(e, AimPoint(e, v.viewProj, (int)sw, (int)sh, cx, cy));
            Vec2 sp;
            if (!W2S(v.viewProj, p, sp, (int)sw, (int)sh)) break;
            const float d = sqrtf((sp.x - cx) * (sp.x - cx) + (sp.y - cy) * (sp.y - cy));
            if (d <= fovPx * 1.5f && (now - g.lastSwitch) * 1000.0 > g_cfg.aimSwitchDelayMs) {
                best = &e; bestPx = sp.x; bestPy = sp.y;
            }
            break;
        }
    }

    if (!best) {
        ReleaseFinger("无目标");
        g.have = false; g_picked = nullptr; g.targetId = 0;
        return;
    }

    if (best->id != g.targetId) {
        g.targetId = best->id;
        g.lastSwitch = now;
        g.lockAt = 0;
    }
    g_picked = best;
    g.have = true;
    g.tgtX = bestPx; g.tgtY = bestPy;

    // 首锁延迟
    if (g.lockAt == 0) g.lockAt = now;
    if ((now - g.lockAt) * 1000.0 < g_cfg.aimDelayMs) return;

    // ---------- 3) 偏差
    const float k = sw / 1080.0f;
    const float dead = g_cfg.aimDeadPx * k;
    float dx = g.tgtX - cx;
    float dy = g.tgtY - cy;
    const float len = sqrtf(dx * dx + dy * dy);
    g.lastDelta = len;
    if (len <= dead) return;   // 已在准星上，按住不动（不抖）

    const float ax = sw * ClampF(g_cfg.aimAnchorX, 0.10f, 0.95f);
    const float ay = sh * ClampF(g_cfg.aimAnchorY, 0.10f, 0.95f);
    const float maxStep = g_cfg.aimMaxStepFrac * sw;
    const float rad = g_cfg.aimDragRadiusR * sw;

    // ---------- 4) 还没按下：落指
    if (!g.down) {
        if ((now - g.lastUp) * 1000.0 < g_cfg.aimRecenterMs) return;   // 回中冷却
        if (!EnsureChannel(now)) {
            // 回落：Java 层 MotionEvent 注入（旧路径，pointerId 受限）
            g.moves += JavaBridge::Nudge(dx / len * (len < maxStep ? len : maxStep),
                                         dy / len * (len < maxStep ? len : maxStep)) ? 1 : 0;
            if (g_cfg.aimDebug) {
                static double s_l = 0;
                if (now - s_l > 0.5) { s_l = now; LOGI("自瞄[Java回落] 偏差=%.1fpx 错误=%s", len, JavaBridge::LastError()); }
            }
            return;
        }
        if (now - g.chanReady < 0.20) return;   // 设备刚创建，等 InputReader 注册完
        if (EvdevTouch::AimTouchDown(ax, ay)) {
            g.down = true; g.x = ax; g.y = ay; g.lastSend = now;
            SetErr("");
        } else {
            SetErr("AimTouchDown 失败");
        }
        return;   // 本帧只落指，下一帧再滑
    }

    // ---------- 5) 已按下：滑
    if ((now - g.lastSend) * 1000.0 < g_cfg.aimSendMs) return;

    const float gain = ClampF(g_cfg.aimGain, 0.05f, 2.0f);
    float stepX = ClampF(dx * gain, -maxStep, maxStep);
    float stepY = ClampF(dy * gain, -maxStep, maxStep);
    if (g_cfg.aimInvertX) stepX = -stepX;
    if (g_cfg.aimInvertY) stepY = -stepY;
    if (fabsf(stepX) < 0.4f && fabsf(stepY) < 0.4f) return;

    float nx = g.x + stepX, ny = g.y + stepY;

    // 滑出活动半径或贴屏幕边 → 抬起回中（下一帧在锚点重按）
    if (fabsf(nx - ax) > rad || fabsf(ny - ay) > rad ||
        nx < 8.f || ny < 8.f || nx > sw - 8.f || ny > sh - 8.f) {
        EvdevTouch::AimTouchUp();
        g.down = false; g.lastUp = now; ++g.recenters;
        if (g_cfg.aimDebug) LOGI("自瞄回中：手指(%.0f,%.0f) 超出半径 %.0f 或贴边", nx, ny, rad);
        return;
    }

    if (EvdevTouch::AimTouchMove(nx, ny)) {
        g.x = nx; g.y = ny; g.lastSend = now; ++g.moves;
    } else {
        SetErr("AimTouchMove 失败");
        g.down = false;
    }

    if (g_cfg.aimDebug) {
        static double s_l = 0;
        if (now - s_l > 0.5) {
            s_l = now;
            LOGI("自瞄 锁id=%llu 偏差=%.1fpx 步=%.1f,%.1f 手指=%.0f,%.0f 投递=%d 回中=%d",
                 (unsigned long long)g.targetId, len, stepX, stepY, g.x, g.y, g.moves, g.recenters);
        }
    }
}

}  // namespace TouchAim
