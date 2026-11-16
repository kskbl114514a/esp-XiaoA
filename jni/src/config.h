// config.h —— 全局运行时配置（菜单直接读写这里）
#pragma once
#include <cstdint>

struct EspConfig {
    // ---------- 菜单 / 交互
    bool  menuOpen        = true;     // 悬浮菜单是否展开
    bool  menuCaptureTouch= true;     // 菜单展开时独占触摸设备（EVIOCGRAB）
    bool  touchPassthrough= true;     // 独占后把「菜单外」的触摸用 /dev/uinput 原样转回系统 → 世界照常操作、菜单照样点
    bool  uiTouchPointer  = true;     // 触摸转 ImGui 指针（关掉则只能用游戏内映射）
    float uiTouchSmooth   = 0.0f;     // 触摸指针平滑（0=关闭，越大越跟手但越抖）
    float uiHitPad        = 14.0f;    // 菜单命中容差（像素，1080p 基准）
    float menuCollapseH   = 0.0f;     // 收起态圆钮半径（0=自动）
    float uiScale         = 1.0f;     // 由 surface 宽度自动换算，菜单里可微调
    int   menuKeyIndex    = 0;        // 预留：外部设备/物理键切换

    // ---------- 透视 ESP
    bool  espEnabled      = true;
    bool  drawBox         = true;
    bool  drawSkeleton    = false;
    bool  drawHealthBar   = true;
    bool  drawDistance    = true;
    bool  drawName        = true;
    bool  drawAimLine     = false;
    bool  drawCrosshair   = true;
    bool  showFov         = false;
    float boxThickness    = 1.6f;
    float boxRatio        = 0.52f;    // 框宽 = 框高 * 该比例（0.4~0.7，原来硬编码 0.52）
    float maxDistance     = 400.0f;   // 超过该距离不画
    float skeletonThick   = 1.4f;
    // ---- v5：来自 JS 的判定字段（camp / aiLevel / liveState）过滤开关
    bool  hideAI          = true;     // 不画 AI/BOT（aiLevel>0）
    bool  hideKnocked     = true;     // 不画倒地（liveState==2）
    bool  onlyEnemy       = false;    // 只画敌人（跳过与本地同 camp 的队友）
    bool  showTag         = false;    // 名字后缀显示 [AI]/[倒] 便于核对数据源
    bool  drawLeadPoint   = true;     // 画 JS 解算的提前量落点（依赖 flags bit4）
    // 颜色（RGBA 0..1）
    float colVisible[4]   = {1.00f, 0.25f, 0.25f, 1.0f};   // 可见（红）
    float colOccluded[4]  = {1.00f, 0.80f, 0.10f, 1.0f};   // 遮挡（黄）
    float colTeam[4]      = {0.25f, 0.85f, 1.00f, 1.0f};   // 队友（青）
    float colSkeleton[4]  = {1.00f, 1.00f, 1.00f, 0.9f};
    float colHealth[4]    = {0.20f, 1.00f, 0.35f, 1.0f};
    float colText[4]      = {1.00f, 1.00f, 1.00f, 1.0f};
    float colFov[4]       = {1.00f, 1.00f, 1.00f, 0.55f};
    float colLead[4]      = {1.00f, 0.35f, 1.00f, 1.0f};   // 提前量落点（品红）

    // ---------- 自瞄
    bool  aimEnabled      = false;     // 触摸自瞄总开关（默认【关】，需在菜单"自瞄"页或 ini 里打开）
    bool  aimOnFireOnly   = true;     // 仅在开火时吸附
    bool  aimVisibleOnly  = true;     // 只锁可见目标
    bool  aimIgnoreTeam   = true;
    bool  aimPredict      = true;     // 预判（子弹下坠/飞行时间）
    int   aimBone         = 0;        // 0=头 1=胸 2=最近骨骼
    float aimFov          = 120.0f;   // 像素半径（按 1080p 归一，运行时 * uiScale）
    float aimSpeed        = 220.0f;   // 度/秒，越大越"假"
    float aimSmooth       = 0.35f;    // 0..1 二次平滑系数
    float aimMaxDist      = 250.0f;   // 米
    float aimSwitchDelayMs= 80.0f;    // 换目标冷却，规避行为检测
    float aimDelayMs      = 0.0f;     // 首次锁定延迟
    float bulletSpeed     = 900.0f;   // 预判用，单位/秒
    float targetHeightHead= 1.65f;    // 头高（米），预判/锁头用
    float targetHeightChest=1.20f;

    // ---------- 自瞄落地（触摸注入链路）
    int   aimPointerId    = 9;        // 注入手指 pointerId，避开真实手指 0；游戏不认多指时改 0
    float aimAnchorX      = 0.62f;    // 注入手指落点（屏宽比例），避开摇杆/开火键/准星
    float aimAnchorY      = 0.42f;
    float aimMaxStepPx    = 60.0f;    // 单帧最大位移（1080p 基准，运行时按屏宽缩放）
    bool  aimDebug        = false;    // 打印锁定与位移日志

    // ---- 触摸自瞄（拖动式）：投递走 /dev/uinput 上的一根独立 slot 虚拟手指 ---- //
    float aimGain         = 0.35f;    // 每帧位移 = 偏差 * gain（>1 容易过冲振荡）
    float aimMaxStepFrac  = 0.012f;   // 单帧最大位移，占屏宽比例
    float aimDragRadiusR  = 0.15f;    // 虚拟手指活动半径，占屏宽比例，超出即抬起回中
    float aimSendMs       = 10.0f;    // 最小投递间隔（毫秒），60 帧下 10ms 约等于每帧
    float aimRecenterMs   = 40.0f;    // 回中后重按冷却（毫秒），太小会被当连点
    float aimDeadPx       = 8.0f;     // 死区（1080p 基准像素），小于它不动手
    bool  aimInvertX      = false;    // 拖了反方向就勾这个
    bool  aimInvertY      = false;
    float fireZoneX       = 0.78f;    // 开火键判定区：x > W*fireZoneX 且 y > H*fireZoneY
    float fireZoneY       = 0.52f;

    // ---------- 子弹追踪（Bullet Tracking，替代自瞄：只做预测/弹道可视化，不自动转向）
    bool  btEnabled       = true;      // 总开关（子弹追踪已恢复，默认开）
    bool  btShowPoint     = true;     // 预测命中点（圈+十字）
    bool  btShowArc       = true;     // 弹道抛物线预览
    bool  btShowInfo      = true;     // 显示距离/飞行时间/提前量
    bool  btUseGravity    = true;     // 计算下坠
    float btGravity       = 9.8f;     // 重力加速度 m/s^2（游戏内可调）
    float btBulletSpeed   = 900.0f;   // 子弹初速 m/s
    bool  btDragModel     = false;    // 空气阻力（远距离低速枪才需要）
    float btDragK         = 0.0006f;  // 阻力系数
    float btMaxDist       = 400.0f;   // 超过不计算
    int   btBone          = 0;        // 0=头 1=胸
    bool  btOnlyInFov     = true;     // 只算准星附近的
    float btFovPx         = 420.0f;   // 屏幕像素半径（1080p 基准）
    float btArcSegments   = 16.0f;    // 弹道分段数
    bool  btLiveCamera    = true;     // 渲染时实时读相机矩阵（消除转视角滞后，核心低延迟项）
    bool  btIgnoreTeam    = true;      // 子弹追踪跳过队友队（与自己同 camp 的实体不参与解算）

    // ---------- 数据源
    int   sensorMode      = 3;        // 0=Native 场景遍历 1=矩阵启发 2=强制外部写入(shm) 3=自动优先JS数据
    bool  sensorDumpScene = false;     // 打开后每 2 秒 dump 一次场景树到 logcat
    float worldScale      = 1.0f;     // 游戏世界单位 → 米，标定用
    int   screenW         = 0;
    int   screenH         = 0;

    // ---------- 调试
    bool  debugOverlay    = false;    // 显示帧耗时/实体数/数据源状态
    bool  logFrames       = false;
    bool  matrixProbe     = false;    // 矩阵探针写盘（仅标定期开，会拖帧）
    bool  predEsp         = true;     // 渲染侧速度外推，补偿快照滞后
    float predLagMax      = 0.15f;    // 最大外推时长（秒）
    int   workerHz        = 0;        // 0=自动(按采集耗时自适应) 120/144/240 可手选
};

extern EspConfig g_cfg;
