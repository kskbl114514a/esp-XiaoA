// touch.cpp —— hook Cocos 的 Java→native 触摸入口
// 为什么 hook 这里而不是 InputDispatcher：
//   1) 不 root、不改系统、不影响其它进程；
//   2) 拿到的就是游戏真正消费的触摸点，坐标与渲染视口一致，ESP/自瞄换算不用二次校准；
//   3) 想吞就吞（不调用原函数），想放就放，菜单与游戏互不干扰。
#include <jni.h>
#include "touch.h"
#include "render.h"
#include "config.h"
#include "hook.h"
#include "log.h"
#include "vecmath.h"

#include <dlfcn.h>
#include <atomic>
#include <cstring>

namespace TouchHook {

typedef void (*FnAction)(JNIEnv*, jclass, jint, jfloat, jfloat);

static FnAction origDown = nullptr, origMove = nullptr, origUp = nullptr, origCancel = nullptr;
static std::atomic<bool> g_installed{false};
static std::atomic<bool> g_fireHeld{false};
static std::atomic<bool> g_consuming{false};

static std::atomic<float> g_mx{0}, g_my{0};
static std::atomic<int>   g_sw{1080}, g_sh{2400};
static std::atomic<bool>  g_menuOpen{true};
static std::atomic<float> g_rect[4] = {{0}, {0}, {0}, {0}};    // 菜单窗口 x,y,w,h
static std::atomic<float> g_toggle[4] = {{0}, {0}, {0}, {0}}; // 唤起按钮

static inline bool InRect(float x, float y, float rx, float ry, float rw, float rh) {
    return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

void SetMenuRect(const Rect& r, bool open) {
    g_rect[0].store(r.x); g_rect[1].store(r.y);
    g_rect[2].store(r.w); g_rect[3].store(r.h);
    g_menuOpen.store(open);
    g_cfg.menuOpen = open;
}
void SetToggleRect(const Rect& r) {
    g_toggle[0].store(r.x); g_toggle[1].store(r.y);
    g_toggle[2].store(r.w); g_toggle[3].store(r.h);
}
Rect GetMenuRect() {
    return Rect{g_rect[0].load(), g_rect[1].load(), g_rect[2].load(), g_rect[3].load()};
}
bool MenuOpenByTouch() { return g_menuOpen.load(); }

// 纯几何命中：点是否落在菜单窗口上（外扩 10px 容差）。与"吞不吞触摸"完全无关。
// evdev 靠它判断"这根手指该不该喂给 ImGui"，因此把它单独暴露出去。
bool InMenuRect(float x, float y) {
    Rect r = GetMenuRect();
    if (r.w <= 1.f || r.h <= 1.f) return false;
    const float pad = (g_cfg.uiHitPad > 0.f ? g_cfg.uiHitPad : 0.f) * Render::UiScale();
    return InRect(x, y, r.x - pad, r.y - pad, r.w + pad * 2.f, r.h + pad * 2.f);
}

// 拦截决策：只有「菜单展开 + 吞触摸开关打开 + 几何命中」才把事件从游戏手里截走
static bool ShouldConsume(float x, float y) {
    if (!g_menuOpen.load()) return false;
    if (!g_cfg.menuCaptureTouch) return false;
    return InMenuRect(x, y);
}

bool HitToggleRect(float x, float y) {
    Rect r{ g_toggle[0].load(), g_toggle[1].load(), g_toggle[2].load(), g_toggle[3].load() };
    if (r.w <= 1.f || r.h <= 1.f) return false;

    if (g_menuOpen.load()) {
        // 展开态：热区 = 「✕ 收起菜单」按钮本身，一格都不外扩。
        // 事故原因（实测）：按钮 y426..556，Tab 栏 y575..615，而 uiHitPad(14)*UiScale(2.96)=41，
        // 热区下沿被撑到 597，把整条 Tab 栏的上半截吞掉 —— 点「自瞄」Tab 变成关菜单，
        // 于是"切不了功能 / 勾不了选项"。这里严禁加 pad。
        return InRect(x, y, r.x, r.y, r.w, r.h);
    }

    // 收起态：左上角圆钮，给足容差（手指没必要点得精准）
    float pad = g_cfg.uiHitPad > 0.f ? g_cfg.uiHitPad : 16.f;
    pad *= Render::UiScale();
    return InRect(x, y, r.x - pad, r.y - pad, r.w + pad * 2.f, r.h + pad * 2.f);
}
static bool HitToggle(float x, float y) { return HitToggleRect(x, y); }

void SetMenuOpen(bool open) { g_menuOpen.store(open); g_cfg.menuOpen = open; }
bool HitMenuRect(float x, float y) { return ShouldConsume(x, y); }
bool MenuCapture() { return g_menuOpen.load() && g_cfg.menuCaptureTouch; }

static void FeedImGui(int action, float x, float y) {
    Render::InjectRawTouch(0, action, x, y);
}

static void Down(JNIEnv* env, jclass c, jint id, jfloat x, jfloat y) {
    g_mx = x; g_my = y;
    const int sw = g_sw.load();
    if (x > sw * 0.5f) g_fireHeld = true;

    if (!g_menuOpen.load() && HitToggle(x, y)) {
        g_menuOpen = true;
        g_cfg.menuOpen = true;
        LOGI("菜单唤出");
        return;                       // 吞掉这次按下
    }
    if (ShouldConsume(x, y)) {
        g_consuming = true;
        FeedImGui(0, x, y);
        return;
    }
    g_consuming = false;
    if (origDown) origDown(env, c, id, x, y);
}

static void Move(JNIEnv* env, jclass c, jint id, jfloat x, jfloat y) {
    g_mx = x; g_my = y;
    if (g_consuming.load() || ShouldConsume(x, y)) {
        FeedImGui(2, x, y);
        return;
    }
    if (origMove) origMove(env, c, id, x, y);
}

static void Up(JNIEnv* env, jclass c, jint id, jfloat x, jfloat y) {
    g_fireHeld = false;
    if (g_consuming.load()) {
        FeedImGui(1, x, y);
        g_consuming = false;
        return;
    }
    if (origUp) origUp(env, c, id, x, y);
}

static void Cancel(JNIEnv* env, jclass c, jint id, jfloat x, jfloat y) {
    g_fireHeld = false;
    if (g_consuming.load()) {
        FeedImGui(3, x, y);
        g_consuming = false;
        return;
    }
    if (origCancel) origCancel(env, c, id, x, y);
}

// ------------------------------------------------------------- 安装
static void* Sym(const char* n) {
    void* p = dlsym(RTLD_DEFAULT, n);
    if (!p) p = dlsym(RTLD_DEFAULT, n);   // 两次尝试，规避部分 ROM 的符号可见性抖动
    return p;
}

bool Install() {
    if (g_installed.load()) return true;

    // 记录当前 surface 尺寸，用于右半屏开火判定
    g_sw = g_cfg.screenW > 0 ? g_cfg.screenW : 1080;
    g_sh = g_cfg.screenH > 0 ? g_cfg.screenH : 2400;

    struct Item { const char* name; void* hookFn; FnAction* orig; };
    static Item items[4] = {
        {"Java_com_cocos_lib_CocosTouchHandler_handleActionDown",   (void*)&Down,   &origDown},
        {"Java_com_cocos_lib_CocosTouchHandler_handleActionMove",   (void*)&Move,   &origMove},
        {"Java_com_cocos_lib_CocosTouchHandler_handleActionUp",     (void*)&Up,     &origUp},
        {"Java_com_cocos_lib_CocosTouchHandler_handleActionCancel", (void*)&Cancel, &origCancel},
    };

    int ok = 0;
    for (auto& it : items) {
        void* target = Sym(it.name);
        if (!target) { LOGW("符号不存在: %s", it.name); continue; }
        void* orig = nullptr;
        if (InstallHook(target, it.hookFn, &orig)) {
            *it.orig = (FnAction)orig;
            ++ok;
        } else {
            LOGE("hook 失败: %s", it.name);
        }
    }
    if (ok == 0) {
        LOGE("触摸接管失败：Cocos 触摸符号一个都没 hook 上");
        return false;
    }
    g_installed = true;
    LOGI("触摸接管已安装 %d/4", ok);
    return true;
}

void Uninstall() {
    if (!g_installed.load()) return;
    const char* names[4] = {
        "Java_com_cocos_lib_CocosTouchHandler_handleActionDown",
        "Java_com_cocos_lib_CocosTouchHandler_handleActionMove",
        "Java_com_cocos_lib_CocosTouchHandler_handleActionUp",
        "Java_com_cocos_lib_CocosTouchHandler_handleActionCancel",
    };
    for (auto n : names) {
        void* t = Sym(n);
        if (t) UninstallHook(t);
    }
    g_installed = false;
    LOGI("触摸接管已卸载");
}

bool IsFireHeld() { return g_fireHeld.load(); }

}  // namespace TouchHook
