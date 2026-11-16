// entry.cpp 鈥斺€?娉ㄥ叆鍏ュ彛锛氭寕閽?eglSwapBuffers锛屽湪娓告垙姣忓抚浜ゆ崲缂撳啿鍖哄墠鎶婅嚜宸辩殑鐢婚潰鍙犱笂鍘?
#include "config.h"
#include "log.h"
#include "hook.h"
#include "render.h"
#include "esp.h"
#include "menu.h"
#include "sensor.h"
#include "bullet_track.h"
#include "touch.h"
#include "evtouch.h"
#include "touchaim.h"
#include "java_bridge.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------- 鍑芥暟鎸囬拡
typedef EGLBoolean (*PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef EGLBoolean (*PFN_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint*);
typedef EGLDisplay (*PFN_eglGetCurrentDisplay)(void);

static PFN_eglSwapBuffers      o_eglSwapBuffers = nullptr;
static PFN_eglQuerySurface     o_eglQuerySurface = nullptr;

static volatile bool g_running = false;
static int  g_frameCount = 0;
static int  g_surfW = 0, g_surfH = 0;

static double NowSec() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---------------------------------------------------------------- 涓€甯?
static void DrawOneFrame(int w, int h) {
    static double last = 0;
    static ISensor* sensor = nullptr;
    static std::vector<Entity> ents;
    double now = NowSec();
    float dt = (last > 0) ? (float)(now - last) : 0.016f;
    last = now;
    if (dt > 0.12f) dt = 0.12f;
    if (dt <= 0.f) dt = 0.016f;

    Render::NewFrame(w, h);
    ImDrawList* fg = ImGui::GetForegroundDrawList();

    if (!sensor) {
        sensor = GetSensor();
        if (sensor && !sensor->Init()) {
            LOGW("数据源 %s 初始化失败", sensor->Name());
        } else if (sensor) {
            LOGI("数据源就绪：%s", sensor->Name());
        }
    }

    // auto 模式（sensorMode==3）：JS 侧 esp_data.bin 出现后自动切到 JS 数据源
    if (g_cfg.sensorMode == 3 && sensor) {
        static double s_lastProbe = -1e9;
        const double tp = NowSec();
        if (tp - s_lastProbe > 2.0) {
            s_lastProbe = tp;
            const bool isShm = (strstr(sensor->Name(), "共享") != nullptr);
            if (!isShm && ProbeShmOnce()) {
                LOGI("检测到 JS 写入的 esp_data.bin，切换到 JS 数据源");
                ResetSensor();
                sensor = GetSensor();
                if (sensor && !sensor->Init()) LOGW("数据源 %s 初始化失败", sensor->Name());
            }
        }
    }

    ViewInfo view;
    ents.clear();
    bool haveData = false;
    if (sensor) haveData = sensor->Update(view, ents);
    // 屏幕尺寸统一以 eglQuerySurface 拿到的真实 surface 为准：
    // 数据源报的 viewW/viewH 可能是游戏内部设计分辨率（模拟器缩放时与 surface 不一致），
    // 一旦不一致，所有屏幕坐标都会系统性偏移。
    if (w > 0 && h > 0) { view.viewW = w; view.viewH = h; }
    // 场景 dump 限频：原来每帧都 dump，120Hz 下等于每秒 120 条 logcat，纯拖帧
    if (sensor && g_cfg.sensorDumpScene) {
        static double s_lastDump = -1e9;
        const double tDump = ImGui::GetTime();
        if (tDump - s_lastDump > 2.0) { s_lastDump = tDump; sensor->Dump(); }
    }

    Esp::Draw(fg, view, ents);
    // 子弹追踪：每帧先解算再绘制，纯可视化，不产生任何输入事件
    Track::Update(view, ents, dt);
    Track::Draw(fg, view);
    // 触摸自瞄：选目标 + 往 /dev/uinput 独立 slot 虚拟手指投递拖动。
    // 放在 Menu::Draw 之前：菜单展开时 TouchAim::Update 内部会先松手指，避免和点菜单抢触摸。
    TouchAim::Update(view, ents, dt);
    Menu::Draw(w, h);
    Esp::DrawWatermark(fg, (float)w, (float)h, (int)ents.size(),
                       sensor ? sensor->Name() : "鏃", dt * 1000.f);
    (void)haveData;

    // ---- 性能埋点（2 秒一次）：定位"菜单卡/画面掉帧"到底掉在哪
    {
        static double s_perfT = -1e9;
        static int    s_perfN = 0;
        static float  s_perfSum = 0.f, s_perfMax = 0.f;
        const float   ms = dt * 1000.f;
        const double  tNow = ImGui::GetTime();
        ++s_perfN; s_perfSum += ms;
        if (ms > s_perfMax) s_perfMax = ms;
        if (tNow - s_perfT > 2.0) {
            const float avg = s_perfN ? s_perfSum / (float)s_perfN : 0.f;
            LOGI("Perf fps=%.1f frame=%.1fms max=%.1fms ents=%zu dev=%s grab=%d pass=%d ptr=%d",
                 s_perfN / 2.0f, avg, s_perfMax, ents.size(),
                 EvdevTouch::DevicePath(), EvdevTouch::Grabbed() ? 1 : 0,
                 EvdevTouch::PassCount(), g_cfg.uiTouchPointer ? 1 : 0);
            s_perfT = tNow; s_perfN = 0; s_perfSum = 0.f; s_perfMax = 0.f;
        }
    }

    Render::EndFrame();

    if (g_cfg.logFrames && (++g_frameCount % 120 == 0))
        LOGI("甯?%d  %dx%d  瀹炰綋 %zu  鏁版嵁婧?%s", g_frameCount, w, h, ents.size(),
             sensor ? sensor->Name() : "鏃");
}

// ---------------------------------------------------------------- eglSwapBuffers hook
static EGLBoolean Hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!o_eglSwapBuffers) return EGL_FALSE;
    if (g_running && surf != EGL_NO_SURFACE) {
        int w = g_surfW, h = g_surfH;
        // eglQuerySurface 每帧查询会打断渲染管线，改为首帧 + 每 60 帧校准一次
        static int q_tick = 0;
        const bool needQuery = (w <= 0 || h <= 0) || ((q_tick++ % 60) == 0);
        if (needQuery && o_eglQuerySurface) {
            EGLint qw = 0, qh = 0;
            if (o_eglQuerySurface(dpy, surf, EGL_WIDTH, &qw) && o_eglQuerySurface(dpy, surf, EGL_HEIGHT, &qh)) {
                if (qw > 0 && qh > 0) { w = qw; h = qh; }
            }
        }
        if (w > 0 && h > 0) {
            g_surfW = w; g_surfH = h;
            g_cfg.screenW = w; g_cfg.screenH = h;
            DrawOneFrame(w, h);
        }
    }
    return o_eglSwapBuffers(dpy, surf);
}

// ---------------------------------------------------------------- 瀹夎
static bool HookEgl() {
    // 娓告垙鑷甫浜嗕竴涓彧鏈?9KB 鐨勮浆鍙?stub libEGL.so锛屽繀椤绘樉寮忔墦寮€绯荤粺瀹炵幇
    const char* paths[] = {
        "/system/lib64/libEGL.so",
        "/system/lib/libEGL.so",
        "/vendor/lib64/libEGL.so",
        "/vendor/lib/libEGL.so",
        nullptr
    };
    void* h = nullptr;
    for (int i = 0; paths[i]; ++i) {
        h = dlopen(paths[i], RTLD_NOW | RTLD_GLOBAL);
        if (h) { LOGI("鎵撳紑 %s", paths[i]); break; }
    }
    if (!h) h = dlopen("libEGL.so", RTLD_NOW);
    if (!h) { LOGE("dlopen libEGL 澶辫触: %s", dlerror()); return false; }

    o_eglSwapBuffers = (PFN_eglSwapBuffers)dlsym(h, "eglSwapBuffers");
    o_eglQuerySurface = (PFN_eglQuerySurface)dlsym(h, "eglQuerySurface");
    if (!o_eglSwapBuffers) { LOGE("鎵句笉鍒?eglSwapBuffers"); return false; }

    void* orig = nullptr;
    if (!InstallHook((void*)o_eglSwapBuffers, (void*)Hook_eglSwapBuffers, &orig)) {
        LOGE("hook eglSwapBuffers 澶辫触");
        return false;
    }
    if (orig) o_eglSwapBuffers = (PFN_eglSwapBuffers)orig;
    LOGI("eglSwapBuffers 宸叉寕閽?orig=%p", orig);
    return true;
}

// NDK 鍏紑 stub 涓嶅鍑?JNI_GetCreatedJavaVMs锛岃繍琛屾椂浠?libart 鍙?
static JavaVM* GetJavaVM() {
    using Fn = jint (*)(JavaVM**, jsize, jsize*);
    static Fn fn = nullptr;
    if (!fn) {
        void* art = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD);
        if (!art) art = dlopen("libart.so", RTLD_NOW);
        if (art) fn = (Fn)dlsym(art, "JNI_GetCreatedJavaVMs");
        if (!fn) {
            void* nh = dlopen("libnativehelper.so", RTLD_NOW);
            if (nh) fn = (Fn)dlsym(nh, "JNI_GetCreatedJavaVMs");
        }
    }
    if (!fn) { LOGE("鎷夸笉鍒?JNI_GetCreatedJavaVMs"); return nullptr; }
    JavaVM* vm = nullptr;
    jsize n = 0;
    if (fn(&vm, 1, &n) == JNI_OK && n > 0) return vm;
    return nullptr;
}

// 鍦烘櫙鑷妇鎺㈤拡锛氬欢杩熸墽琛岋紝閬垮厤涓庢父鎴忛甯ц祫婧愬姞杞芥姠 CPU
static void* WorkThread(void*) {
    LOGI("宸ヤ綔绾跨▼鍚姩锛岀瓑寰呮父鎴忔覆鏌撶幆澧冣€");

#if defined(__x86_64__)
    // x86_64 娉ㄥ叆锛氭父鎴忕殑 libcocos.so 鏄?arm64 guest 搴擄紝x86_64 linker 鍛藉悕绌洪棿閲?
// dlopen 涓嶅埌锛屽洜姝や笉绛夊畠锛岀洿鎺ユ寕 x86_64 鐨?eglSwapBuffers銆?
// 娓告垙渚?arm64 鐨?EGL 璋冪敤浼氱粡 houdini 杞彂鍒板悓涓€杩涚▼閲岀殑 x86_64 libEGL锛宧ook 鑳藉懡涓€?
for (int i = 0; i < 50 && !g_running; ++i) {
        if (HookEgl()) { g_running = true; break; }
        LOGW("hook EGL 绗?%d 娆″け璐ワ紝200ms 鍚庨噸璇", i + 1);
        usleep(200 * 1000);
    }
#else
    // 绛?libcocos 瀹屽叏鍔犺浇锛堟父鎴忎富寰幆璺戣捣鏉ュ啀鎸傦紝閬垮厤鏃╂湡鍒濆鍖栭樁娈佃鑷繁鐨?hook 骞叉壈锛?
for (int i = 0; i < 600 && !g_running; ++i) {
        void* cocos = dlopen("libcocos.so", RTLD_NOW);
        if (cocos) {
            if (HookEgl()) g_running = true;
            break;
        }
        usleep(100 * 1000);
    }
#endif
    if (!g_running) { LOGE("娓叉煋鐜鏈氨缁紝閫€鍑"); return nullptr; }

    // 瑙︽懜鎺ョ锛堣彍鍗曚氦浜?+ 寮€鐏垽瀹氾級
    sleep(1);
    if (!TouchHook::Install()) {
        // houdini 鐜涓?arm64 guest 鐨?Java_xxx 瑙︽懜绗﹀彿涓嶅彲瑙侊紝鏀硅蛋 /dev/input 鍘熷浜嬩欢
        EvdevTouch::Start();
    }

    // 鑷瀯瑙︽懜娉ㄥ叆
    JavaVM* vm = GetJavaVM();
    if (vm) {
        JNIEnv* env = nullptr;
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            JavaBridge::Init(env, nullptr);
            vm->DetachCurrentThread();
        }
    } else {
        LOGW("鏈彇寰?JavaVM锛岃嚜鐬勮Е鎽告敞鍏ヤ笉鍙敤");
    }

    Menu::Load();
    g_cfg.menuOpen = true;
    LOGI("鍒濆鍖栧畬鎴?鈥斺€?鑿滃崟宸插睍寮€");
    return nullptr;
}

__attribute__((constructor))
static void OnLoad() {
    LOGI("esp.so 宸茶浇鍏?(pid=%d)锛屾瀯寤轰紭鍖栧紑鍚", getpid());
    pthread_t th;
    if (pthread_create(&th, nullptr, WorkThread, nullptr) == 0) {
        pthread_detach(th);
    } else {
        LOGE("宸ヤ綔绾跨▼鍒涘缓澶辫触");
    }
}

__attribute__((destructor))
static void OnUnload() {
    LOGI("esp.so 鍗歌浇锛屽洖鏀?hook");
    g_running = false;
    TouchAim::Reset();          // 卸载前必须先把虚拟手指抬起来，否则会留一根"永远按着"的手指
    TouchHook::Uninstall();
    EvdevTouch::Stop();
    Render::Shutdown();
}

