// evtouch.cpp —— /dev/input/event* 多点触摸通道（x86_64 / houdini 兜底）
// ---------------------------------------------------------------------------
// 旧实现的三个致命问题（这就是"菜单点不动 / 收不起来"的根因）：
//   1) 只用单点状态机（curX/curY/fingerDown），多点触摸下 slot 交织，坐标来回跳；
//   2) 喂 ImGui 的条件是 HitMenuRect()，而它内部要求 menuCaptureTouch==true
//      → 一旦关掉"菜单吞触摸"，ImGui 一个事件都收不到，菜单彻底死掉；
//   3) 展开态没有任何"收起"通道，只能指望 ImGui 标题栏那个 10px 的叉。
//
// 新模型（三档，由 menuCaptureTouch + touchPassthrough 组合控制）：
//   A. 独占 + 穿透（默认）：EVIOCGRAB 抢占物理设备 → 菜单手指喂 ImGui，
//      其余手指用 /dev/uinput 建虚拟触摸设备原样转回系统 → 世界照常转视角/移动。
//      效果：菜单点得动，同时不"吞掉"整个屏幕的触摸。
//   B. 独占 + 不穿透：全吞，游戏完全静止（原行为，硬核模式）。
//   C. 不独占：完全放行物理设备，ImGui 只镜像"落在菜单矩形内"的手指
//      （代价：点菜单时游戏也会收到那份触摸）。
// ---------------------------------------------------------------------------
#include "evtouch.h"
#include "touch.h"
#include "render.h"
#include "config.h"
#include "log.h"

#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>

#if defined(__has_include)
#  if __has_include(<linux/uinput.h>)
#    include <linux/uinput.h>
#    define HAVE_UINPUT_HDR 1
#  endif
#endif
#ifndef HAVE_UINPUT_HDR
// NDK 头缺失时的最小定义（内核 ABI 稳定，可直接照抄）
#define UINPUT_IOCTL_BASE 'U'
struct uinput_user_dev {
    char name[80];
    struct input_id id;
    unsigned int ff_effects_max;
    int absmax[ABS_CNT], absmin[ABS_CNT], absfuzz[ABS_CNT], absflat[ABS_CNT];
};
#define UI_DEV_CREATE   _IO(UINPUT_IOCTL_BASE, 1)
#define UI_DEV_DESTROY  _IO(UINPUT_IOCTL_BASE, 2)
#define UI_SET_EVBIT    _IOW(UINPUT_IOCTL_BASE, 100, int)
#define UI_SET_KEYBIT   _IOW(UINPUT_IOCTL_BASE, 101, int)
#define UI_SET_ABSBIT   _IOW(UINPUT_IOCTL_BASE, 103, int)
#define UI_SET_PROPBIT  _IOW(UINPUT_IOCTL_BASE, 110, int)
#endif

namespace EvdevTouch {

static const int MAX_SLOTS = 12;
// 自瞄虚拟手指专用槽位：物理手指只会落在 0..MAX_SLOTS-2（穿透重放也是这套编号），
// 给它单独留最后一槽，重放逻辑跳过它，两边永不打架。
static const int AIM_SLOT   = MAX_SLOTS - 1;
static const int AIM_TID    = 300;

struct SlotState {
    int   trackId   = -1;
    int   rx        = -1, ry = -1;   // 物理原始坐标
    bool  down      = false;
    bool  prevDown  = false;
    float sx        = 0, sy = 0;     // 映射到 surface 像素
    float sentX     = 0, sentY = 0;  // 已喂给 ImGui 的最后坐标
    // uinput 重放状态
    bool  vDown     = false;
    int   vx        = -1, vy = -1;
};

static SlotState g_slot[MAX_SLOTS];
static int  g_curSlot = 0;     // ABS_MT_SLOT 指定的当前槽
static int  g_uiSlot  = -1;    // 已被 ImGui 占用的手指（同一时刻只允许一根）
static bool g_btnTouch = false;
static bool g_hasTracking = false;

static std::atomic<bool>  g_running{false};
static std::atomic<bool>  g_grabbed{false};
static std::atomic<bool>  g_fireHeld{false};
static std::atomic<int>   g_fd{-1};
static std::atomic<float> g_lastX{0}, g_lastY{0};
static int  g_maxX = 3199, g_maxY = 1439;
static char g_path[64] = "-";

static int  g_ufd = -1;                 // uinput 虚拟设备
static char g_upath[64] = "-";
static std::atomic<int> g_passCount{0}; // 转回系统的手指事件帧数（诊断用）

// 自瞄虚拟手指状态（写 uinput 与穿透重放共用一把锁）
static std::atomic<bool> g_aimDown{false};
static float g_aimX = 0.f, g_aimY = 0.f;
static pthread_mutex_t g_uMtx = PTHREAD_MUTEX_INITIALIZER;

static inline bool TestBit(const unsigned long* bits, int bit) {
    return (bits[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1u;
}

// ---------------------------------------------------------------- uinput
static bool UInputCreate() {
    if (g_ufd >= 0) return true;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { LOGW("uinput 打不开: %s（root 侧 chmod 666 /dev/uinput）", strerror(errno)); return false; }

    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
    ioctl(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);
    ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof(ud));
    snprintf(ud.name, sizeof(ud.name), "esp_vtouch");
    ud.id.bustype = BUS_VIRTUAL;
    ud.id.vendor = 0x0e5e; ud.id.product = 0x2026; ud.id.version = 1;
    ud.absmin[ABS_MT_POSITION_X] = 0; ud.absmax[ABS_MT_POSITION_X] = g_maxX;
    ud.absmin[ABS_MT_POSITION_Y] = 0; ud.absmax[ABS_MT_POSITION_Y] = g_maxY;
    ud.absmin[ABS_MT_SLOT] = 0;       ud.absmax[ABS_MT_SLOT] = MAX_SLOTS - 1;
    ud.absmin[ABS_MT_TRACKING_ID] = 0; ud.absmax[ABS_MT_TRACKING_ID] = 65535;
    ud.absmin[ABS_MT_TOUCH_MAJOR] = 0; ud.absmax[ABS_MT_TOUCH_MAJOR] = 255;
    ud.absmin[ABS_MT_PRESSURE] = 0;    ud.absmax[ABS_MT_PRESSURE] = 255;
    if (write(fd, &ud, sizeof(ud)) != (ssize_t)sizeof(ud)) {
        LOGE("uinput 写 uinput_user_dev 失败: %s", strerror(errno));
        close(fd); return false;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        LOGE("UI_DEV_CREATE 失败: %s", strerror(errno));
        close(fd); return false;
    }
    g_ufd = fd;
    LOGI("uinput 虚拟触摸设备已创建（%s），范围 %dx%d", ud.name, g_maxX, g_maxY);
    return true;
}

static void UInputDestroy() {
    if (g_ufd < 0) return;
    ioctl(g_ufd, UI_DEV_DESTROY);
    close(g_ufd);
    g_ufd = -1;
    LOGI("uinput 虚拟触摸设备已销毁");
}

static inline void Emit(int type, int code, int val) {
    struct input_event e;
    memset(&e, 0, sizeof(e));
    e.type = (__u16)type; e.code = (__u16)code; e.value = val;
    if (write(g_ufd, &e, sizeof(e)) < 0) { /* 设备被拔/满了，忽略 */ }
}
static inline void EmitSyn() { Emit(EV_SYN, SYN_REPORT, 0); }

// 把"不属于菜单"的手指原样转回系统，实现物理独占下的区域穿透
static void UInputFlush() {
    if (g_ufd < 0) return;
    pthread_mutex_lock(&g_uMtx);
    // 只有真的独占了物理设备才需要转回：没独占时系统本来就收得到原始事件，
    // 再转一遍会让游戏里出现「双击 / 多出一根手指」（实测 pass 计数每次都涨）。
    const bool grabbed = g_grabbed.load();
    bool changed = false, anyDown = g_aimDown.load();
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (i == AIM_SLOT) continue;      // 自瞄手指自己管，别被重放覆盖
        SlotState& s = g_slot[i];
        const bool pass = grabbed && s.down && i != g_uiSlot && s.rx >= 0 && s.ry >= 0;
        if (pass) {
            if (!s.vDown) {
                Emit(EV_ABS, ABS_MT_SLOT, i);
                Emit(EV_ABS, ABS_MT_TRACKING_ID, 100 + i);
                Emit(EV_ABS, ABS_MT_POSITION_X, s.rx);
                Emit(EV_ABS, ABS_MT_POSITION_Y, s.ry);
                Emit(EV_ABS, ABS_MT_TOUCH_MAJOR, 8);
                Emit(EV_ABS, ABS_MT_PRESSURE, 128);
                s.vDown = true; changed = true;
            } else if (s.vx != s.rx || s.vy != s.ry) {
                Emit(EV_ABS, ABS_MT_SLOT, i);
                Emit(EV_ABS, ABS_MT_POSITION_X, s.rx);
                Emit(EV_ABS, ABS_MT_POSITION_Y, s.ry);
                changed = true;
            }
            s.vx = s.rx; s.vy = s.ry; anyDown = true;
        } else if (s.vDown) {
            Emit(EV_ABS, ABS_MT_SLOT, i);
            Emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
            s.vDown = false; changed = true;
        }
    }
    if (changed) {
        Emit(EV_KEY, BTN_TOUCH, anyDown ? 1 : 0);
        EmitSyn();
        ++g_passCount;
    }
    pthread_mutex_unlock(&g_uMtx);
}

// ---------------------------------------------------------------- 自瞄虚拟手指
// 独立 slot：与物理手指（0..MAX_SLOTS-2）完全隔离，游戏侧看到的是「另一根手指」，
// 不会跟玩家真实手指抢 pointerId=0，也不需要 Java 层 MotionEvent 注入。
static inline int UnmapX(float sx) {
    const int sw = g_cfg.screenW > 0 ? g_cfg.screenW : (g_maxX + 1);
    int v = (int)(sx * (float)(g_maxX > 0 ? g_maxX : 1) / (float)(sw > 0 ? sw : 1));
    if (v < 0) v = 0;
    if (v > g_maxX) v = g_maxX;
    return v;
}
static inline int UnmapY(float sy) {
    const int sh = g_cfg.screenH > 0 ? g_cfg.screenH : (g_maxY + 1);
    int v = (int)(sy * (float)(g_maxY > 0 ? g_maxY : 1) / (float)(sh > 0 ? sh : 1));
    if (v < 0) v = 0;
    if (v > g_maxY) v = g_maxY;
    return v;
}

bool AimTouchActive() { return g_aimDown.load(); }

bool AimTouchReady() {
    pthread_mutex_lock(&g_uMtx);
    const bool ok = UInputCreate();
    pthread_mutex_unlock(&g_uMtx);
    return ok;
}

bool AimTouchDown(float sx, float sy) {
    pthread_mutex_lock(&g_uMtx);
    if (!UInputCreate()) { pthread_mutex_unlock(&g_uMtx); return false; }
    if (g_aimDown.load()) { pthread_mutex_unlock(&g_uMtx); return true; }
    const int rx = UnmapX(sx), ry = UnmapY(sy);
    g_aimX = sx; g_aimY = sy;
    Emit(EV_ABS, ABS_MT_SLOT, AIM_SLOT);
    Emit(EV_ABS, ABS_MT_TRACKING_ID, AIM_TID);
    Emit(EV_ABS, ABS_MT_POSITION_X, rx);
    Emit(EV_ABS, ABS_MT_POSITION_Y, ry);
    Emit(EV_ABS, ABS_MT_TOUCH_MAJOR, 8);
    Emit(EV_ABS, ABS_MT_PRESSURE, 128);
    Emit(EV_KEY, BTN_TOUCH, 1);
    EmitSyn();
    g_aimDown = true;
    pthread_mutex_unlock(&g_uMtx);
    return true;
}

bool AimTouchMove(float sx, float sy) {
    pthread_mutex_lock(&g_uMtx);
    if (g_ufd < 0 || !g_aimDown.load()) { pthread_mutex_unlock(&g_uMtx); return false; }
    const int rx = UnmapX(sx), ry = UnmapY(sy);
    if (rx == UnmapX(g_aimX) && ry == UnmapY(g_aimY)) { pthread_mutex_unlock(&g_uMtx); return true; }
    g_aimX = sx; g_aimY = sy;
    Emit(EV_ABS, ABS_MT_SLOT, AIM_SLOT);
    Emit(EV_ABS, ABS_MT_POSITION_X, rx);
    Emit(EV_ABS, ABS_MT_POSITION_Y, ry);
    EmitSyn();
    pthread_mutex_unlock(&g_uMtx);
    return true;
}

bool AimTouchUp() {
    pthread_mutex_lock(&g_uMtx);
    if (g_ufd < 0 || !g_aimDown.load()) { pthread_mutex_unlock(&g_uMtx); return false; }
    Emit(EV_ABS, ABS_MT_SLOT, AIM_SLOT);
    Emit(EV_ABS, ABS_MT_TRACKING_ID, -1);
    // 物理手指还有按住的话不能把 BTN_TOUCH 清掉
    bool anyPhys = false;
    for (int i = 0; i < MAX_SLOTS; ++i) if (i != AIM_SLOT && g_slot[i].vDown) { anyPhys = true; break; }
    Emit(EV_KEY, BTN_TOUCH, anyPhys ? 1 : 0);
    EmitSyn();
    g_aimDown = false;
    pthread_mutex_unlock(&g_uMtx);
    return true;
}

// ---------------------------------------------------------------- 设备
static int OpenTouchDevice() {
    for (int i = 0; i < 24; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        unsigned long absBits[(ABS_MAX / (8 * sizeof(unsigned long))) + 2] = {0};
        unsigned long propBits[(INPUT_PROP_MAX / (8 * sizeof(unsigned long))) + 2] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) < 0 ||
            !TestBit(absBits, ABS_MT_POSITION_X) || !TestBit(absBits, ABS_MT_POSITION_Y)) {
            close(fd); continue;
        }
        ioctl(fd, EVIOCGPROP(sizeof(propBits)), propBits);
        struct input_absinfo ax{}, ay{};
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
        if (ax.maximum <= 0 || ay.maximum <= 0) { close(fd); continue; }
        g_maxX = ax.maximum; g_maxY = ay.maximum;
        g_hasTracking = TestBit(absBits, ABS_MT_TRACKING_ID);
        snprintf(g_path, sizeof(g_path), "%s", path);
        LOGI("evdev 触摸设备 %s 范围 %dx%d direct=%d tracking=%d",
             path, g_maxX, g_maxY, TestBit(propBits, INPUT_PROP_DIRECT) ? 1 : 0, g_hasTracking ? 1 : 0);
        return fd;
    }
    return -1;
}

static inline float MapX(int v) {
    const int sw = g_cfg.screenW > 0 ? g_cfg.screenW : (g_maxX + 1);
    return (float)v * (float)sw / (float)(g_maxX > 0 ? g_maxX : 1);
}
static inline float MapY(int v) {
    const int sh = g_cfg.screenH > 0 ? g_cfg.screenH : (g_maxY + 1);
    return (float)v * (float)sh / (float)(g_maxY > 0 ? g_maxY : 1);
}

static void Grab(bool on) {
    int fd = g_fd.load();
    if (fd < 0) return;
    if (on == g_grabbed.load()) return;
    if (ioctl(fd, EVIOCGRAB, on ? 1 : 0) == 0) {
        g_grabbed = on;
        LOGI("evdev %s 触摸独占 %s", g_path, on ? "开启" : "释放");
        if (on && g_cfg.touchPassthrough) {
            pthread_mutex_lock(&g_uMtx);
            UInputCreate();
            pthread_mutex_unlock(&g_uMtx);
        }
        if (!on) UInputFlush();  // 释放前把残留手指收尾
    } else {
        LOGW("evdev EVIOCGRAB(%d) 失败 errno=%d", on ? 1 : 0, errno);
    }
}

// ---------------------------------------------------------------- 菜单命中
static inline bool MenuOpen() { return TouchHook::MenuOpenByTouch(); }

// 这一下按下是否归 ImGui。
// 唯一判据是「手指是否落在菜单矩形内」，而不是"菜单一开就全给 ImGui"——
// 后者会让独占模式下第一根手指（右手转视角/开火那根）被菜单吃掉，
// 表现为"菜单能点，但人动不了/视角乱飘"。
static bool TakeForUi(float sx, float sy) {
    if (!g_cfg.uiTouchPointer) return false;
    if (!MenuOpen()) return false;
    return TouchHook::InMenuRect(sx, sy);
}

// ---------------------------------------------------------------- 结算
static void Commit() {
    // 无 TRACKING_ID 的设备用 BTN_TOUCH 推导按下状态
    if (!g_hasTracking) {
        for (int i = 0; i < MAX_SLOTS; ++i) {
            g_slot[i].down = (i == 0) && g_btnTouch && g_slot[i].rx >= 0 && g_slot[i].ry >= 0;
        }
    }
    for (int i = 0; i < MAX_SLOTS; ++i) {
        SlotState& s = g_slot[i];
        if (s.rx >= 0 && s.ry >= 0) { s.sx = MapX(s.rx); s.sy = MapY(s.ry); }
    }

    // 1) UI 手指抬起
    if (g_uiSlot >= 0 && !g_slot[g_uiSlot].down) {
        SlotState& s = g_slot[g_uiSlot];
        Render::InjectRawTouch(0, 1, s.sentX, s.sentY);
        LOGI("UI up %.0f,%.0f", s.sentX, s.sentY);
        g_uiSlot = -1;
    }

    // 2) 新按下：先判菜单热区（收起/唤出），再判 UI 归属
    for (int i = 0; i < MAX_SLOTS; ++i) {
        SlotState& s = g_slot[i];
        if (!(s.down && !s.prevDown)) continue;
        if (s.rx < 0 || s.ry < 0) continue;

        if (TouchHook::HitToggleRect(s.sx, s.sy)) {
            const bool wasOpen = MenuOpen();
            TouchHook::SetMenuOpen(!wasOpen);
            LOGI("evdev 热区 %s菜单 at %.0f,%.0f", wasOpen ? "收起" : "唤出", s.sx, s.sy);
            // 立刻同步独占状态，别等下一轮 read
            Grab(MenuOpen() && g_cfg.menuCaptureTouch);
            continue;                     // 这一下不喂 ImGui
        }
        if (g_uiSlot < 0 && TakeForUi(s.sx, s.sy)) {
            g_uiSlot = i;
            s.sentX = s.sx; s.sentY = s.sy;
            Render::InjectRawTouch(0, 0, s.sx, s.sy);
            LOGI("UI down %.0f,%.0f slot=%d", s.sx, s.sy, i);
            continue;
        }
        // 开火手（右下角）判定：不抢 UI，只做标记
        const float fx = g_cfg.screenW * (g_cfg.fireZoneX > 0.01f ? g_cfg.fireZoneX : 0.78f);
        const float fy = g_cfg.screenH * (g_cfg.fireZoneY > 0.01f ? g_cfg.fireZoneY : 0.52f);
        if (s.sx > fx && s.sy > fy) g_fireHeld = true;
    }

    // 3) UI 手指移动
    if (g_uiSlot >= 0) {
        SlotState& s = g_slot[g_uiSlot];
        if (s.down) {
            const float dx = s.sx - s.sentX, dy = s.sy - s.sentY;
            const float th = 0.5f + g_cfg.uiTouchSmooth * 3.0f;
            if (fabsf(dx) > th || fabsf(dy) > th) {
                s.sentX = s.sx; s.sentY = s.sy;
                g_lastX = s.sx; g_lastY = s.sy;
                Render::InjectRawTouch(0, 2, s.sx, s.sy);
            }
        }
    }

    // 4) 释放检测
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (g_slot[i].prevDown && !g_slot[i].down && i != g_uiSlot) {
            const float fx = g_cfg.screenW * (g_cfg.fireZoneX > 0.01f ? g_cfg.fireZoneX : 0.78f);
            const float fy = g_cfg.screenH * (g_cfg.fireZoneY > 0.01f ? g_cfg.fireZoneY : 0.52f);
            if (g_slot[i].sx > fx && g_slot[i].sy > fy) g_fireHeld = false;
        }
        g_slot[i].prevDown = g_slot[i].down;
    }
}

// ---------------------------------------------------------------- 事件处理
static void HandleEvents(int fd) {
    struct input_event evs[64];

    while (g_running.load() && g_fd.load() == fd) {
        // 独占策略：菜单展开且选择了"独占"才 Grab
        const bool wantGrab = MenuOpen() && g_cfg.menuCaptureTouch;
        if (wantGrab != g_grabbed.load()) Grab(wantGrab);
        if (!g_cfg.touchPassthrough && !g_aimDown.load() && g_ufd >= 0) UInputDestroy();
        // uinput 懒重建：首次因权限失败时，不重建就永远穿透不了
        if (g_cfg.touchPassthrough && wantGrab && g_ufd < 0) {
            static double s_nextTry = 0;
            const double t = (double)time(nullptr);
            if (t >= s_nextTry) { s_nextTry = t + 3.0; UInputCreate(); }
        }

        ssize_t n = read(fd, evs, sizeof(evs));
        if (n <= 0) {
            if (n < 0 && (errno == ENODEV || errno == EBADF)) return;
            usleep(8 * 1000);
            continue;
        }
        const int cnt = (int)(n / (ssize_t)sizeof(struct input_event));
        for (int i = 0; i < cnt; ++i) {
            struct input_event& e = evs[i];
            if (e.type == EV_ABS) {
                switch (e.code) {
                    case ABS_MT_SLOT:
                        g_curSlot = (e.value >= 0 && e.value < MAX_SLOTS) ? e.value : 0;
                        break;
                    case ABS_MT_TRACKING_ID:
                        g_hasTracking = true;
                        if (e.value < 0) g_slot[g_curSlot].down = false;
                        else { g_slot[g_curSlot].trackId = e.value; g_slot[g_curSlot].down = true; }
                        break;
                    case ABS_MT_POSITION_X: g_slot[g_curSlot].rx = e.value; break;
                    case ABS_MT_POSITION_Y: g_slot[g_curSlot].ry = e.value; break;
                    default: break;
                }
            } else if (e.type == EV_KEY && e.code == BTN_TOUCH) {
                g_btnTouch = (e.value != 0);
                if (!g_btnTouch) {
                    for (int k = 0; k < MAX_SLOTS; ++k) if (!g_hasTracking) g_slot[k].down = false;
                }
            } else if (e.type == EV_SYN && e.code == SYN_REPORT) {
                Commit();
                UInputFlush();
            }
        }
    }
}

// ---------------------------------------------------------------- 线程
static void* Thread(void*) {
    while (g_running.load()) {
        int fd = g_fd.load();
        if (fd < 0) {
            fd = OpenTouchDevice();
            if (fd < 0) {
                static int warn = 0;
                if (warn++ % 3 == 0)
                    LOGW("evdev 尚未拿到触摸设备，3 秒后重试（root 侧需 chmod 666 /dev/input/event*）");
                sleep(3);
                continue;
            }
            g_fd = fd;
            memset(g_slot, 0, sizeof(g_slot));
            g_uiSlot = -1;
        }
        HandleEvents(fd);
        Grab(false);
        if (!g_aimDown.load()) UInputDestroy();   // 自瞄正按住时保住虚拟设备
        int old = g_fd.exchange(-1);
        if (old >= 0) close(old);
        if (g_running.load()) sleep(1);
    }
    return nullptr;
}

bool Start() {
    if (g_running.load()) return true;
    g_running = true;
    pthread_t th;
    if (pthread_create(&th, nullptr, Thread, nullptr) != 0) {
        g_running = false;
        LOGE("evdev 线程创建失败");
        return false;
    }
    pthread_detach(th);
    LOGI("evdev 触摸通道已启动（后台扫描 /dev/input）");
    return true;
}

void Stop() {
    if (!g_running.exchange(false)) return;
    Grab(false);
    UInputDestroy();
    usleep(50 * 1000);
    int fd = g_fd.exchange(-1);
    if (fd >= 0) close(fd);
    LOGI("evdev 触摸通道已停止");
}

bool Running() { return g_running.load(); }
const char* DevicePath() { return g_path; }
bool FireHeld() { return g_fireHeld.load(); }
float LastX() { return g_lastX.load(); }
float LastY() { return g_lastY.load(); }
bool Grabbed() { return g_grabbed.load(); }
int  PassCount() { return g_passCount.load(); }
const char* UInputPath() { return g_upath; }

}  // namespace EvdevTouch
