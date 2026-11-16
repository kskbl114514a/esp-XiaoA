// menu.cpp —— 悬浮菜单：透视 / 子弹追踪 / 触摸自瞄 / 配置持久化
#include "menu.h"
#include "menu.h"
#include "config.h"
#include "render.h"
#include "touch.h"
#include "evtouch.h"
#include "touchaim.h"
#include "log.h"
#include "vecmath.h"   // ClampF

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <unistd.h>   // fsync / fileno / access

namespace Menu {

static char  g_status[160] = "";
static float g_panelPos[2] = {20.f, 120.f};

// ---------------------------------------------------------- 配置持久化
// 包名不再写死：从 /proc/self/cmdline 取进程名（即包名），
// 换游戏/换渠道包时不用再改源码（原实现把 com.fmgame.pubg.aligames 硬编码在路径里）。
static void SelfPkg(char* out, size_t n) {
    out[0] = 0;
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (!f) return;
    size_t r = fread(out, 1, n - 1, f);
    fclose(f);
    out[r] = 0;
    for (size_t i = 0; i < r; ++i) if (out[i] == '\0') { out[i] = 0; break; }
}

// 主路径：<外部私有目录>/esp_cfg.ini；备用路径：/data/local/tmp/esp_cfg.ini
// 主路径不可写（部分包没给 WRITE_EXTERNAL 权限 / Android 11 分区存储）时自动落备用路径。
static const char* CfgPath() {
    static char p[256] = {0};
    if (p[0]) return p;
    char pkg[128] = {0};
    SelfPkg(pkg, sizeof(pkg));
    if (!pkg[0]) snprintf(pkg, sizeof(pkg), "com.fmgame.pubg.aligames");
    snprintf(p, sizeof(p), "/sdcard/Android/data/%s/files/esp_cfg.ini", pkg);
    return p;
}
static const char* CfgPathAlt() { return "/data/local/tmp/esp_cfg.ini"; }

// 原子写：先写 <path>.tmp，成功后 rename 覆盖。
// 原实现直接 fopen(path,"w")，进程在写一半被杀就会留下半个 ini，下次加载直接把配置读成默认值。
void Save() {
    char tmp[300];
    const char* real = nullptr;
    snprintf(tmp, sizeof(tmp), "%s.tmp", CfgPath());
    FILE* f = fopen(tmp, "w");
    if (f) real = CfgPath();
    if (!f) {
        snprintf(tmp, sizeof(tmp), "%s.tmp", CfgPathAlt());
        f = fopen(tmp, "w");
        if (f) real = CfgPathAlt();
    }
    if (!f) {
        snprintf(g_status, sizeof(g_status), "保存失败：%s 与 %s 均不可写", CfgPath(), CfgPathAlt());
        return;
    }
    fprintf(f, "espEnabled=%d\ndrawBox=%d\ndrawSkeleton=%d\ndrawHealthBar=%d\ndrawDistance=%d\n"
               "drawName=%d\ndrawCrosshair=%d\nboxThickness=%.2f\nboxRatio=%.3f\n"
               "maxDistance=%.1f\nskeletonThick=%.2f\n"
               "worldScale=%.3f\nuiScale=%.2f\nmenuCaptureTouch=%d\ntouchPassthrough=%d\n",
            (int)g_cfg.espEnabled, (int)g_cfg.drawBox, (int)g_cfg.drawSkeleton, (int)g_cfg.drawHealthBar,
            (int)g_cfg.drawDistance, (int)g_cfg.drawName, (int)g_cfg.drawCrosshair,
            g_cfg.boxThickness, g_cfg.boxRatio, g_cfg.maxDistance, g_cfg.skeletonThick,
            g_cfg.worldScale, g_cfg.uiScale, (int)g_cfg.menuCaptureTouch, (int)g_cfg.touchPassthrough);
    fprintf(f, "colVisible=%.3f,%.3f,%.3f\ncolOccluded=%.3f,%.3f,%.3f\ncolTeam=%.3f,%.3f,%.3f\n",
            g_cfg.colVisible[0], g_cfg.colVisible[1], g_cfg.colVisible[2],
            g_cfg.colOccluded[0], g_cfg.colOccluded[1], g_cfg.colOccluded[2],
            g_cfg.colTeam[0], g_cfg.colTeam[1], g_cfg.colTeam[2]);
    fprintf(f, "drawAimLine=%d\nshowFov=%d\ndebugOverlay=%d\nsensorMode=%d\n",
            (int)g_cfg.drawAimLine, (int)g_cfg.showFov, (int)g_cfg.debugOverlay, g_cfg.sensorMode);
    fprintf(f, "btEnabled=%d\nbtShowPoint=%d\nbtShowArc=%d\nbtShowInfo=%d\n"
               "btUseGravity=%d\nbtDragModel=%d\nbtOnlyInFov=%d\nbtLiveCamera=%d\n"
               "btBone=%d\nbtIgnoreTeam=%d\n",
            (int)g_cfg.btEnabled, (int)g_cfg.btShowPoint, (int)g_cfg.btShowArc, (int)g_cfg.btShowInfo,
            (int)g_cfg.btUseGravity, (int)g_cfg.btDragModel, (int)g_cfg.btOnlyInFov,
            (int)g_cfg.btLiveCamera, g_cfg.btBone, (int)g_cfg.btIgnoreTeam);
    // ---- v5：JS 判定字段相关的显示/过滤开关
    fprintf(f, "hideAI=%d\nhideKnocked=%d\nonlyEnemy=%d\nshowTag=%d\ndrawLeadPoint=%d\n",
            (int)g_cfg.hideAI, (int)g_cfg.hideKnocked, (int)g_cfg.onlyEnemy,
            (int)g_cfg.showTag, (int)g_cfg.drawLeadPoint);
    fprintf(f, "colLead=%.3f,%.3f,%.3f\n",
            g_cfg.colLead[0], g_cfg.colLead[1], g_cfg.colLead[2]);
    fprintf(f, "btGravity=%.3f\nbtBulletSpeed=%.1f\nbtDragK=%.6f\nbtMaxDist=%.1f\n"
               "btFovPx=%.1f\nbtArcSegments=%.1f\n",
            g_cfg.btGravity, g_cfg.btBulletSpeed, g_cfg.btDragK, g_cfg.btMaxDist,
            g_cfg.btFovPx, g_cfg.btArcSegments);
    // ---- 触摸自瞄：早期版本根本没把这些键写进 ini，菜单里调完重启就全丢
    fprintf(f, "aimEnabled=%d\naimOnFireOnly=%d\naimVisibleOnly=%d\naimIgnoreTeam=%d\n"
               "aimPredict=%d\naimDebug=%d\naimInvertX=%d\naimInvertY=%d\naimBone=%d\n"
               "aimFov=%.1f\naimMaxDist=%.1f\naimGain=%.3f\naimMaxStepFrac=%.4f\n"
               "aimDeadPx=%.1f\naimSendMs=%.1f\naimSwitchDelayMs=%.1f\naimDelayMs=%.1f\n"
               "aimRecenterMs=%.1f\naimAnchorX=%.3f\naimAnchorY=%.3f\naimDragRadiusR=%.3f\n",
            (int)g_cfg.aimEnabled, (int)g_cfg.aimOnFireOnly, (int)g_cfg.aimVisibleOnly,
            (int)g_cfg.aimIgnoreTeam, (int)g_cfg.aimPredict, (int)g_cfg.aimDebug,
            (int)g_cfg.aimInvertX, (int)g_cfg.aimInvertY, g_cfg.aimBone,
            g_cfg.aimFov, g_cfg.aimMaxDist, g_cfg.aimGain, g_cfg.aimMaxStepFrac,
            g_cfg.aimDeadPx, g_cfg.aimSendMs, g_cfg.aimSwitchDelayMs, g_cfg.aimDelayMs,
            g_cfg.aimRecenterMs, g_cfg.aimAnchorX, g_cfg.aimAnchorY, g_cfg.aimDragRadiusR);
    fflush(f);
    fsync(fileno(f));            // 掉电/强杀后不留空文件
    fclose(f);
    if (rename(tmp, real) != 0) {
        snprintf(g_status, sizeof(g_status), "保存失败：rename %s", tmp);
        return;
    }
    snprintf(g_status, sizeof(g_status), "已保存 → %s", real);
}

void Load() {
    const char* used = CfgPath();
    FILE* f = fopen(used, "r");
    if (!f) { used = CfgPathAlt(); f = fopen(used, "r"); }
    if (!f) return;
    int unknown = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* k = line;
        char* vv = eq + 1;
        while (*vv == ' ' || *vv == '\t') ++vv;
        // 去掉行尾 \r\n，否则「键值比较」在 CRLF 文件上会整片失配（Windows 端手改 ini 就会踩）
        size_t vl = strlen(vv);
        while (vl && (vv[vl - 1] == '\n' || vv[vl - 1] == '\r')) vv[--vl] = 0;
        const char* v = vv;
        float n = (float)atof(v);
        int   b = (int)atoi(v);
#define KBOOL(key, field) if (!strcmp(k, key)) { g_cfg.field = (b != 0); continue; }
#define KF(key, field)    if (!strcmp(k, key)) { g_cfg.field = n; continue; }
#define KINT(key, field)  if (!strcmp(k, key)) { g_cfg.field = b; continue; }
        KBOOL("espEnabled", espEnabled)
        KBOOL("drawBox", drawBox)
        KBOOL("drawSkeleton", drawSkeleton)
        KBOOL("drawHealthBar", drawHealthBar)
        KBOOL("drawDistance", drawDistance)
        KBOOL("drawName", drawName)
        KBOOL("drawCrosshair", drawCrosshair)
        KBOOL("drawAimLine", drawAimLine)
        KBOOL("showFov", showFov)
        KBOOL("menuCaptureTouch", menuCaptureTouch)
        KBOOL("touchPassthrough", touchPassthrough)
        KBOOL("debugOverlay", debugOverlay)
        // ---- 子弹追踪
        KBOOL("btEnabled", btEnabled)
        KBOOL("btShowPoint", btShowPoint)
        KBOOL("btShowArc", btShowArc)
        KBOOL("btShowInfo", btShowInfo)
        KBOOL("btUseGravity", btUseGravity)
        KBOOL("btDragModel", btDragModel)
        KBOOL("btOnlyInFov", btOnlyInFov)
        KBOOL("btLiveCamera", btLiveCamera)
        KBOOL("btIgnoreTeam", btIgnoreTeam)
        KINT("btBone", btBone)
        KINT("sensorMode", sensorMode)
        // ---- v5：JS 判定字段相关的过滤/显示开关
        KBOOL("hideAI", hideAI)
        KBOOL("hideKnocked", hideKnocked)
        KBOOL("onlyEnemy", onlyEnemy)
        KBOOL("showTag", showTag)
        KBOOL("drawLeadPoint", drawLeadPoint)
        KF("btGravity", btGravity)
        KF("btBulletSpeed", btBulletSpeed)
        KF("btDragK", btDragK)
        KF("btMaxDist", btMaxDist)
        KF("btFovPx", btFovPx)
        KF("btArcSegments", btArcSegments)
        KF("boxThickness", boxThickness)
        KF("boxRatio", boxRatio)
        KF("maxDistance", maxDistance)
        KF("skeletonThick", skeletonThick)
        KF("worldScale", worldScale)
        KF("uiScale", uiScale)
        // ---- 触摸自瞄
        KBOOL("aimEnabled", aimEnabled)
        KBOOL("aimOnFireOnly", aimOnFireOnly)
        KBOOL("aimVisibleOnly", aimVisibleOnly)
        KBOOL("aimIgnoreTeam", aimIgnoreTeam)
        KBOOL("aimPredict", aimPredict)
        KBOOL("aimDebug", aimDebug)
        KBOOL("aimInvertX", aimInvertX)
        KBOOL("aimInvertY", aimInvertY)
        KINT("aimBone", aimBone)
        KF("aimFov", aimFov)
        KF("aimMaxDist", aimMaxDist)
        KF("aimGain", aimGain)
        KF("aimMaxStepFrac", aimMaxStepFrac)
        KF("aimDeadPx", aimDeadPx)
        KF("aimSendMs", aimSendMs)
        KF("aimSwitchDelayMs", aimSwitchDelayMs)
        KF("aimDelayMs", aimDelayMs)
        KF("aimRecenterMs", aimRecenterMs)
        KF("aimAnchorX", aimAnchorX)
        KF("aimAnchorY", aimAnchorY)
        KF("aimDragRadiusR", aimDragRadiusR)
#undef KBOOL
#undef KF
#undef KINT
        if (!strncmp(k, "col", 3)) {
            float r = 0, g = 0, bl = 0;
            if (sscanf(v, "%f,%f,%f", &r, &g, &bl) == 3) {
                float* dst = nullptr;
                if (!strcmp(k, "colVisible")) dst = g_cfg.colVisible;
                else if (!strcmp(k, "colOccluded")) dst = g_cfg.colOccluded;
                else if (!strcmp(k, "colTeam")) dst = g_cfg.colTeam;
                else if (!strcmp(k, "colLead")) dst = g_cfg.colLead;
                if (!dst) ++unknown;          // 形如 colXXX 却对不上任何颜色字段
                else { dst[0] = r; dst[1] = g; dst[2] = bl; }
            }
        } else {
            ++unknown;                        // 未知键不再静默丢弃，加载完统一报数量
        }
    }
    fclose(f);

    // ---- 取值夹紧：配置文件是可以手改的，越界值（btBone=7 / uiScale=0 / 负距离）
    //      原来会被原样喂进渲染与解算，轻则不画、重则除零与框体飞出屏幕。
    if (g_cfg.btBone < 0) g_cfg.btBone = 0;
    if (g_cfg.btBone > 1) g_cfg.btBone = 1;                 // 只有 0=头 1=胸 两个骨骼
    if (g_cfg.sensorMode < 0) g_cfg.sensorMode = 0;
    if (g_cfg.sensorMode > 3) g_cfg.sensorMode = 3;
    g_cfg.boxThickness   = ClampF(g_cfg.boxThickness, 0.2f, 8.f);
    g_cfg.boxRatio       = ClampF(g_cfg.boxRatio, 0.20f, 1.20f);
    g_cfg.skeletonThick  = ClampF(g_cfg.skeletonThick, 0.2f, 8.f);
    g_cfg.maxDistance    = ClampF(g_cfg.maxDistance, 0.f, 3000.f);
    g_cfg.btMaxDist      = ClampF(g_cfg.btMaxDist, 1.f, 3000.f);
    g_cfg.btBulletSpeed  = ClampF(g_cfg.btBulletSpeed, 50.f, 3000.f);
    g_cfg.btGravity      = ClampF(g_cfg.btGravity, 0.f, 40.f);
    g_cfg.btDragK        = ClampF(g_cfg.btDragK, 0.f, 0.01f);
    g_cfg.btFovPx        = ClampF(g_cfg.btFovPx, 10.f, 4000.f);
    g_cfg.btArcSegments  = ClampF(g_cfg.btArcSegments, 4.f, 48.f);
    g_cfg.uiScale        = ClampF(g_cfg.uiScale, 0.5f, 3.0f);
    if (!(g_cfg.worldScale > 0.001f) || g_cfg.worldScale > 1000.f) g_cfg.worldScale = 1.f;
    if (!(g_cfg.btBulletSpeed > 1.f)) g_cfg.btBulletSpeed = 900.f;

    // 自瞄不再无条件强关：默认值来自 config.h（false），要开只能从菜单或 ini 显式打开。
    // 但必须夹住参数，否则 ini 里 aimGain=50 这种值会直接把手指甩出屏幕。
    g_cfg.aimBone          = (g_cfg.aimBone < 0 || g_cfg.aimBone > 2) ? 0 : g_cfg.aimBone;
    g_cfg.aimFov           = ClampF(g_cfg.aimFov, 10.f, 800.f);
    g_cfg.aimGain          = ClampF(g_cfg.aimGain, 0.05f, 2.0f);
    g_cfg.aimMaxDist       = ClampF(g_cfg.aimMaxDist, 0.f, 1500.f);
    g_cfg.aimSwitchDelayMs = ClampF(g_cfg.aimSwitchDelayMs, 0.f, 1000.f);
    g_cfg.aimDelayMs       = ClampF(g_cfg.aimDelayMs, 0.f, 2000.f);
    g_cfg.aimAnchorX       = ClampF(g_cfg.aimAnchorX, 0.05f, 0.95f);
    g_cfg.aimAnchorY       = ClampF(g_cfg.aimAnchorY, 0.05f, 0.95f);
    g_cfg.aimMaxStepFrac   = ClampF(g_cfg.aimMaxStepFrac, 0.001f, 0.10f);
    g_cfg.aimDragRadiusR   = ClampF(g_cfg.aimDragRadiusR, 0.02f, 0.50f);
    g_cfg.aimSendMs        = ClampF(g_cfg.aimSendMs, 5.f, 200.f);
    g_cfg.aimRecenterMs    = ClampF(g_cfg.aimRecenterMs, 0.f, 500.f);
    g_cfg.aimDeadPx        = ClampF(g_cfg.aimDeadPx, 0.f, 60.f);
    LOGI("配置已加载 %s（未知键 %d 个）", used, unknown);
}

// ---------------------------------------------------------- 透视页
static void DrawEspPage() {
    ImGui::Checkbox("启用透视", &g_cfg.espEnabled);
    ImGui::SameLine();
    ImGui::Checkbox("显示准星", &g_cfg.drawCrosshair);
    ImGui::Separator();

    ImGui::Checkbox("方框", &g_cfg.drawBox);
    ImGui::SameLine();
    ImGui::Checkbox("骨骼", &g_cfg.drawSkeleton);
    ImGui::SameLine();
    ImGui::Checkbox("血条", &g_cfg.drawHealthBar);
    ImGui::Checkbox("距离", &g_cfg.drawDistance);
    ImGui::SameLine();
    ImGui::Checkbox("名字", &g_cfg.drawName);
    ImGui::SameLine();
    ImGui::Checkbox("提前量点", &g_cfg.drawLeadPoint);
    ImGui::Separator();

    ImGui::TextUnformatted("数据过滤 ← JS 透传 camp/aiLevel/liveState");
    ImGui::Checkbox("屏蔽 AI", &g_cfg.hideAI);
    ImGui::SameLine();
    ImGui::Checkbox("屏蔽倒地", &g_cfg.hideKnocked);
    ImGui::SameLine();
    ImGui::Checkbox("仅敌人", &g_cfg.onlyEnemy);
    ImGui::Checkbox("显示原始标记", &g_cfg.showTag);
    ImGui::Separator();

    ImGui::SliderFloat("线宽", &g_cfg.boxThickness, 0.5f, 5.0f, "%.1f");
    ImGui::SliderFloat("骨骼线宽", &g_cfg.skeletonThick, 0.5f, 4.0f, "%.1f");
    ImGui::SliderFloat("最大绘制距离", &g_cfg.maxDistance, 20.f, 1500.f, "%.0f");
    ImGui::Separator();

    ImGui::ColorEdit3("可见(敌)", g_cfg.colVisible, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::ColorEdit3("遮挡", g_cfg.colOccluded, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::ColorEdit3("队友", g_cfg.colTeam, ImGuiColorEditFlags_NoInputs);

    // ---- 子弹追踪（纯预测可视化：只画命中点/弹道弧/提前量，不产生输入事件）
    if (ImGui::CollapsingHeader("子弹追踪", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("启用追踪", &g_cfg.btEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("命中点", &g_cfg.btShowPoint);
        ImGui::SameLine();
        ImGui::Checkbox("弹道弧", &g_cfg.btShowArc);
        ImGui::Checkbox("数据显示", &g_cfg.btShowInfo);
        ImGui::SameLine();
        ImGui::Checkbox("算下坠", &g_cfg.btUseGravity);
        ImGui::SameLine();
        ImGui::Checkbox("仅准星附近", &g_cfg.btOnlyInFov);
        ImGui::SliderFloat("弹速(m/s)", &g_cfg.btBulletSpeed, 100.f, 1200.f, "%.0f");
        ImGui::SliderFloat("重力", &g_cfg.btGravity, 0.f, 20.f, "%.1f");
        ImGui::SliderFloat("最大解算距离", &g_cfg.btMaxDist, 20.f, 1200.f, "%.0f");
        ImGui::SliderFloat("准星半径(px)", &g_cfg.btFovPx, 60.f, 1200.f, "%.0f");
        ImGui::SliderFloat("弧线分段", &g_cfg.btArcSegments, 4.f, 48.f, "%.0f");
        int bone = (g_cfg.btBone >= 0 && g_cfg.btBone <= 1) ? g_cfg.btBone : 0;
        if (ImGui::Combo("瞄准骨骼", &bone, "头\0胸\0")) g_cfg.btBone = bone;
        ImGui::TextDisabled("锁头：头点缺失时用脚点推算，不会退化成锁胸");
        ImGui::Checkbox("跳过队友", &g_cfg.btIgnoreTeam);
        ImGui::SliderFloat("框宽比例", &g_cfg.boxRatio, 0.20f, 1.20f, "%.2f");
        ImGui::Checkbox("空气阻力", &g_cfg.btDragModel);
        if (g_cfg.btDragModel) {
            ImGui::SameLine();
            ImGui::SliderFloat("阻力k", &g_cfg.btDragK, 0.f, 0.005f, "%.5f");
        }
    }

    // ---- 触摸自瞄（真·输入投递：往 /dev/uinput 挂一根独立虚拟手指，需要 root/权限）
    if (ImGui::CollapsingHeader("触摸自瞄")) {
        ImGui::Text("状态: %s   通道: %s   投递: %d 帧",
                    TouchAim::StateText(),
                    TouchAim::ChannelOk() ? "uinput 就绪" : "未就绪",
                    TouchAim::MoveFrames());
        if (TouchAim::LastError()[0])
            ImGui::TextColored(ImVec4(1.f, 0.55f, 0.35f, 1.f), "错误: %s", TouchAim::LastError());
        ImGui::TextDisabled("uinput 不可用时回落 Java MotionEvent 注入（pointerId 受限）");
        ImGui::Separator();

        ImGui::Checkbox("启用自瞄", &g_cfg.aimEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("仅开火时", &g_cfg.aimOnFireOnly);
        ImGui::SameLine();
        ImGui::Checkbox("只看可见", &g_cfg.aimVisibleOnly);
        ImGui::Checkbox("跳过队友", &g_cfg.aimIgnoreTeam);
        ImGui::SameLine();
        ImGui::Checkbox("预判提前量", &g_cfg.aimPredict);
        ImGui::SameLine();
        ImGui::Checkbox("调试日志", &g_cfg.aimDebug);

        int ab = (g_cfg.aimBone >= 0 && g_cfg.aimBone <= 2) ? g_cfg.aimBone : 0;
        if (ImGui::Combo("瞄准部位", &ab, "头\0胸\0最近骨骼\0")) g_cfg.aimBone = ab;
        ImGui::SliderFloat("锁定半径(px)", &g_cfg.aimFov, 30.f, 400.f, "%.0f");
        ImGui::SliderFloat("最大距离(m)", &g_cfg.aimMaxDist, 20.f, 600.f, "%.0f");
        ImGui::SliderFloat("跟手增益", &g_cfg.aimGain, 0.05f, 1.2f, "%.2f");
        ImGui::SliderFloat("单帧步长(%屏宽)", &g_cfg.aimMaxStepFrac, 0.002f, 0.05f, "%.3f");
        ImGui::SliderFloat("死区(px)", &g_cfg.aimDeadPx, 0.f, 40.f, "%.0f");
        ImGui::SliderFloat("投递间隔(ms)", &g_cfg.aimSendMs, 5.f, 80.f, "%.0f");
        ImGui::SliderFloat("换目标冷却(ms)", &g_cfg.aimSwitchDelayMs, 0.f, 500.f, "%.0f");
        ImGui::SliderFloat("首锁延迟(ms)", &g_cfg.aimDelayMs, 0.f, 500.f, "%.0f");
        ImGui::SliderFloat("回中冷却(ms)", &g_cfg.aimRecenterMs, 0.f, 300.f, "%.0f");
        ImGui::SliderFloat("落点X(%屏宽)", &g_cfg.aimAnchorX, 0.10f, 0.90f, "%.2f");
        ImGui::SliderFloat("落点Y(%屏高)", &g_cfg.aimAnchorY, 0.10f, 0.90f, "%.2f");
        ImGui::SliderFloat("活动半径(%屏宽)", &g_cfg.aimDragRadiusR, 0.03f, 0.40f, "%.2f");
        ImGui::Checkbox("X 反向", &g_cfg.aimInvertX);
        ImGui::SameLine();
        ImGui::Checkbox("Y 反向", &g_cfg.aimInvertY);
        ImGui::TextDisabled("拖反方向就勾反向；增益过大会过冲抖动");
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("数据源")) {
        static const char* kMode[] = {"场景遍历(自动)", "矩阵启发", "JS写入数据", "自动优先JS"};
        int m = g_cfg.sensorMode;
        if (m < 0 || m > 3) m = 3;
        if (ImGui::Combo("来源", &m, kMode, 4)) g_cfg.sensorMode = m;
        ImGui::TextDisabled("自动优先JS = 有 esp_data.bin 就用 JS 数据，否则回落场景遍历");
    }

    ImGui::Separator();
    ImGui::SliderFloat("世界缩放", &g_cfg.worldScale, 0.02f, 10.0f, "%.3f");
    ImGui::TextDisabled("视口: %d x %d", g_cfg.screenW, g_cfg.screenH);

    if (ImGui::CollapsingHeader("界面 / 触摸")) {
        ImGui::SliderFloat("界面缩放", &g_cfg.uiScale, 0.5f, 2.5f, "%.2f");
        ImGui::Checkbox("独占触摸设备", &g_cfg.menuCaptureTouch);
        ImGui::SameLine();
        ImGui::TextDisabled("(关=完全不抢触摸)");
        ImGui::Checkbox("区域穿透(菜单外手指还给游戏)", &g_cfg.touchPassthrough);
        ImGui::Checkbox("触摸当鼠标指针", &g_cfg.uiTouchPointer);
        ImGui::SliderFloat("命中容差(px)", &g_cfg.uiHitPad, 0.f, 60.f, "%.0f");
        ImGui::SliderFloat("收起圆钮半径", &g_cfg.menuCollapseH, 0.f, 80.f, "%.0f   (0=自动)");
        ImGui::Text("中文字体: %s", Render::FontHasCjk() ? Render::FontPath() : "未加载(中文会变问号)");
        ImGui::TextDisabled("物理设备: %s  独占: %s",
                            EvdevTouch::DevicePath(), EvdevTouch::Grabbed() ? "是" : "否");
    }

    ImGui::Separator();
    if (ImGui::Button("保存配置")) Save();
    ImGui::SameLine();
    if (ImGui::Button("读取配置")) Load();
}

bool Draw(int w, int h) {
    if (!g_cfg.menuOpen) {
        // 收起态：左上角圆钮
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float s = Render::UiScale();
        const ImVec2 p(18.f * s, 150.f * s);
        const float r = (g_cfg.menuCollapseH > 4.f ? g_cfg.menuCollapseH : 30.f) * s;
        ImU32 bg = ImGui::ColorConvertFloat4ToU32(ImVec4(0.10f, 0.55f, 0.85f, 0.72f));
        dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, bg, 32);
        dl->AddCircle(ImVec2(p.x + r, p.y + r), r, IM_COL32(255, 255, 255, 200), 32, 2.f);
        const float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 3.0f);
        dl->AddCircle(ImVec2(p.x + r, p.y + r), r + 4.f * s + pulse * 3.f * s,
                      IM_COL32(80, 200, 255, (int)(60 + 60 * pulse)), 40, 2.f * s);
        const char* t = "透视";
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(18.f * s, FLT_MAX, 0.f, t);
        dl->AddText(ImGui::GetFont(), 18.f * s,
                    ImVec2(p.x + r - ts.x * 0.5f, p.y + r - ts.y * 0.5f),
                    IM_COL32(255, 255, 255, 245), t);
        TouchHook::SetToggleRect(TouchHook::Rect{p.x, p.y, r * 2, r * 2});
        TouchHook::SetMenuRect(TouchHook::Rect{0, 0, 0, 0}, false);
        return false;
    }

    const float s = Render::UiScale();
    ImGui::SetNextWindowPos(ImVec2(g_panelPos[0] * s, g_panelPos[1] * s), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.f, 0.f), ImGuiCond_FirstUseEver);
    ImGui::Begin("王牌突击队 · 透视", &g_cfg.menuOpen,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

    // 收起按钮：同时作为 evdev 原生热区
    TouchHook::Rect collapseRect{0, 0, 0, 0};
    {
        const float bh = 30.f * s;
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.72f, 0.18f, 0.20f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.26f, 0.28f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f, 0.12f, 0.14f, 1.00f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
        if (ImGui::Button("✕  收起菜单", ImVec2(ImGui::GetContentRegionAvail().x > 40.f
                                               ? ImGui::GetContentRegionAvail().x : 200.f, bh))) {
            g_cfg.menuOpen = false;
            TouchHook::SetMenuOpen(false);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        const ImVec2 bmin = ImGui::GetItemRectMin();
        const ImVec2 bmax = ImGui::GetItemRectMax();
        collapseRect = TouchHook::Rect{bmin.x, bmin.y, bmax.x - bmin.x, bmax.y - bmin.y};
        ImGui::Dummy(ImVec2(0.f, 6.f * s));
    }

    DrawEspPage();

    if (g_status[0]) { ImGui::Separator(); ImGui::TextWrapped("%s", g_status); }
    ImGui::Separator();
    ImGui::TextDisabled("收起后左上角『透视』圆钮可再次打开");

    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();
    g_panelPos[0] = wp.x / (s > 0.01f ? s : 1.f);
    g_panelPos[1] = wp.y / (s > 0.01f ? s : 1.f);
    ImGui::End();

    TouchHook::SetMenuRect(TouchHook::Rect{wp.x, wp.y, ws.x, ws.y}, g_cfg.menuOpen);
    TouchHook::SetToggleRect(collapseRect);
    return g_cfg.menuOpen;
}

}  // namespace Menu
