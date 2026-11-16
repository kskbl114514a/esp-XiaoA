// render.cpp
#include "render.h"
#include "config.h"
#include "log.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"

#include <cstring>
#include <cstdio>
#include <mutex>
#include <vector>
#include <sys/stat.h>
#include <ctime>

namespace Render {

static bool  g_inited = false;
static bool  g_enabled = true;
static int   g_w = 0, g_h = 0;
static float g_scale = 1.f;
static char  g_glsl[32] = "#version 300 es";
static char  g_fontPath[256] = {0};
static bool  g_fontCjk = false;

// ------------------------------------------------------------ 输入队列
struct RawEvent { int pointerId; int action; float x, y; };
static std::mutex g_evMtx;
static std::vector<RawEvent> g_events;

static const char* kFontCandidates[] = {
    "/system/fonts/NotoSansCJK-Regular.ttc",
    "/system/fonts/NotoSansCJKsc-Regular.otf",
    "/system/fonts/NotoSansSC-Regular.otf",
    "/system/fonts/SourceHanSansCN-Regular.otf",
    "/system/fonts/SourceHanSansSC-Regular.otf",
    "/system/fonts/DroidSansChinese.ttf",
    "/system/fonts/DroidSansFallback.ttf",
    "/system/fonts/MSYH.TTC",
    "/sdcard/esp_font.ttf",
};

static bool FileExists(const char* p) {
    struct stat st{};
    return stat(p, &st) == 0 && st.st_size > 1024;
}

static bool QueryGLVersion() {
    const char* ver = (const char*)glGetString(GL_VERSION);
    if (!ver) return false;
    LOGI("GL_VERSION=%s", ver);
    if (strstr(ver, "OpenGL ES 3")) { snprintf(g_glsl, sizeof(g_glsl), "%s", "#version 300 es"); return true; }
    snprintf(g_glsl, sizeof(g_glsl), "%s", "#version 100");
    return true;
}

bool Init() {
    if (g_inited) return true;
    if (!QueryGLVersion()) { LOGE("拿不到 GL 版本，跳过 ImGui 初始化"); return false; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;     // 不写 imgui.ini（只读 /data 目录会失败）
    io.LogFilename = nullptr;
    io.BackendPlatformName = "android_egl_overlay";

    // ---- 中文字体：优先系统 CJK，失败则提示
    ImFontConfig fc;
    fc.OversampleH = 2;
    fc.OversampleV = 2;
    fc.PixelSnapH = true;
    for (const char* p : kFontCandidates) {
        if (!FileExists(p)) continue;
        ImFont* f = io.Fonts->AddFontFromFileTTF(p, 26.0f, &fc, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (f) {
            strncpy(g_fontPath, p, sizeof(g_fontPath) - 1);
            g_fontCjk = true;
            LOGI("中文字体加载成功: %s", p);
            break;
        }
        LOGW("字体加载失败: %s", p);
    }
    if (!g_fontCjk) {
        io.Fonts->AddFontDefault();
        LOGW("未找到中文字体，菜单中的中文会显示为 '?'。把任意中文 TTF 放到 /sdcard/esp_font.ttf 后重启即可。");
    }
    io.Fonts->Build();

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding  = 4.0f;
    st.GrabRounding   = 4.0f;
    st.ScrollbarRounding = 4.0f;
    st.WindowBorderSize = 1.0f;
    st.FrameBorderSize  = 0.0f;
    st.WindowPadding = ImVec2(10, 10);
    st.ItemSpacing   = ImVec2(8, 7);
    ImGui::StyleColorsDark();
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.06f, 0.07f, 0.09f, 0.94f);
    c[ImGuiCol_TitleBg]       = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.18f, 0.24f, 1.00f);
    c[ImGuiCol_Header]        = ImVec4(0.18f, 0.32f, 0.48f, 0.85f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.40f, 0.58f, 0.90f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.30f, 0.85f, 0.55f, 1.00f);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.30f, 0.75f, 0.95f, 1.00f);
    c[ImGuiCol_Button]        = ImVec4(0.16f, 0.22f, 0.30f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.32f, 0.44f, 1.00f);

    if (!ImGui_ImplOpenGL3_Init(g_glsl)) {
        LOGE("ImGui_ImplOpenGL3_Init 失败 (%s)", g_glsl);
        ImGui::DestroyContext();
        return false;
    }
    g_inited = true;
    LOGI("ImGui 就绪 (%s)", g_glsl);
    return true;
}

void Shutdown() {
    if (!g_inited) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    g_inited = false;
}

void SetEnabled(bool on) { g_enabled = on; }
bool Enabled() { return g_enabled; }

float UiScale() {
    float s = g_scale * (g_cfg.uiScale > 0.1f ? g_cfg.uiScale : 1.f);
    return s < 0.4f ? 0.4f : s;
}

static std::atomic<int> g_evDrop{0};   // 被丢弃的普通事件数（诊断用）
void PushTouch(const TouchEvent& ev) { InjectRawTouch(0, ev.action, ev.x, ev.y); }

void InjectRawTouch(int pointerId, int action, float x, float y) {
    std::lock_guard<std::mutex> lk(g_evMtx);
    if (g_events.size() >= 128) {
        // 满了不能无脑丢：抬手/取消一旦被丢，ImGui 会永久停在按下状态
        // （滑块粘手、按钮常亮、后面所有点击都错位）。
        const bool critical = (action == 1 || action == 3);
        if (!critical) { ++g_evDrop; return; }              // 普通 MOVE 丢了无所谓
        for (auto it = g_events.begin(); it != g_events.end(); ++it) {
            if (it->action == 2) { g_events.erase(it); break; }   // 腾掉一条 MOVE 保 UP
            }
    }
    g_events.push_back({pointerId, action, x, y});
}

static double NowSec() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// 触摸 → ImGui 指针的核心约束（旧实现就是栽在这里）：
//   1) 触摸点按是"down 与 up 同一帧到齐"，ImGui 的 ButtonBehavior 要求按钮
//      至少有一帧处于 MouseDown 状态才判定为点击。所以同一帧内 down+up 必须
//      把 up 延后一帧发，否则滑块拖不动、按钮点不亮。
//   2) 必须给 DeltaTime，否则双击计数 / 输入时间戳全用默认 1/60，行为飘。
//   3) 必须声明 MouseSource = TouchScreen，hover 语义才对。
static bool  g_holdDown   = false;   // ImGui 侧当前是否处于按下
static bool  g_pendingUp  = false;   // 待延后一帧发送的抬起
static float g_pendingUpX = 0.f, g_pendingUpY = 0.f;
static double g_lastFrameT = 0.0;

static void ApplyInput() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_w, (float)g_h);
    io.DisplayFramebufferScale = ImVec2(1, 1);

    const double now = NowSec();
    float dt = (g_lastFrameT > 0.0) ? (float)(now - g_lastFrameT) : (1.f / 60.f);
    g_lastFrameT = now;
    if (dt < 0.0005f) dt = 0.0005f;
    if (dt > 0.25f)   dt = 0.25f;
    io.DeltaTime = dt;

    // 软件光标：手指位置在菜单上可视化，坐标映射对不对一眼就看出来
    static bool s_cursorSet = false;
    if (!s_cursorSet) {
        io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
        io.ConfigFlags  |= ImGuiConfigFlags_NoMouseCursorChange;
        io.MouseDrawCursor = true;
        s_cursorSet = true;
    }

    // 上一帧遗留的抬起先发掉（保证 down 至少存活一帧）
    if (g_pendingUp) {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(g_pendingUpX, g_pendingUpY);
        io.AddMouseButtonEvent(0, false);
        g_pendingUp = false;
        g_holdDown  = false;
    }

    std::vector<RawEvent> evs;
    {
        std::lock_guard<std::mutex> lk(g_evMtx);
        evs.swap(g_events);
    }
    for (auto& e : evs) {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(e.x, e.y);
        if (e.action == 0) {
            io.AddMouseButtonEvent(0, true);
            g_holdDown = true;
        } else if (e.action == 1 || e.action == 3) {
            if (g_holdDown) {
                // 本帧刚按下就抬起 → 延后到下一帧，让按钮/滑块有机会响应
                g_pendingUp = true;
                g_pendingUpX = e.x; g_pendingUpY = e.y;
            }
        }
    }
}

void NewFrame(int surfaceW, int surfaceH) {
    if (!g_enabled) return;
    if (!Init()) { g_enabled = false; return; }
    if (surfaceW > 0 && surfaceH > 0 && (surfaceW != g_w || surfaceH != g_h)) {
        g_w = surfaceW; g_h = surfaceH;
        g_scale = (float)surfaceW / 1080.0f;
        if (g_scale < 0.5f) g_scale = 0.5f;
        g_cfg.screenW = surfaceW;
        g_cfg.screenH = surfaceH;
        LOGI("surface=%dx%d scale=%.2f", surfaceW, surfaceH, g_scale);
    }
    if (g_w == 0) { g_w = surfaceW; g_h = surfaceH; g_scale = (float)surfaceW / 1080.0f; }

    // 保持游戏状态干净：ESP 需要在深度测试关闭、无裁剪的状态下画线
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, g_w, g_h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    ImGui_ImplOpenGL3_NewFrame();
    ApplyInput();
    // 全局 UI 缩放（只在与上次不同时重算，ScaleAllSizes 会累加，不能每帧调）
    static ImGuiStyle s_base;
    static float s_applied = -1.f;
    if (s_applied < 0.f) s_base = ImGui::GetStyle();
    float sc = UiScale();
    if (fabsf(sc - s_applied) > 0.001f) {
        ImGui::GetStyle() = s_base;
        ImGui::GetStyle().ScaleAllSizes(sc);
        s_applied = sc;
    }
    ImGui::GetIO().FontGlobalScale = 1.0f;   // 字号已在字体加载时按 26px 定死
    ImGui::NewFrame();
}

void EndFrame() {
    if (!g_enabled) return;
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd && dd->CmdListsCount > 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, g_w, g_h);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        ImGui_ImplOpenGL3_RenderDrawData(dd);
    }
}

const char* FontPath() { return g_fontPath; }
bool FontHasCjk() { return g_fontCjk; }

}  // namespace Render
