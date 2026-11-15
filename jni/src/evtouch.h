// evtouch.h —— 直接读 /dev/input 的触摸通道（x86_64 / houdini 兜底）
// 菜单展开时可 EVIOCGRAB 独占物理设备，并用 /dev/uinput 把「菜单外」的手指
// 原样转回系统：菜单点得动，世界也照常操作（区域穿透）。
#pragma once

namespace EvdevTouch {

// 扫描 /dev/input/event*，找到带 ABS_MT_POSITION_X/Y 的触摸设备并起读线程
bool Start();
void Stop();
bool Running();
const char* DevicePath();   // 命中的设备路径（未命中返回 "-"）

// 当前手指位置（surface 像素）
bool FireHeld();
float LastX();
float LastY();

// 菜单是否已独占触摸设备
bool Grabbed();
// 已转回系统的触摸帧数（诊断用）
int  PassCount();
// uinput 虚拟设备名（未创建返回 "-"）
const char* UInputPath();

// ---- 自瞄投递通道：独立 slot 的虚拟手指，与玩家真实手指并存 ----
// 说明：JavaBridge 的 MotionEvent 注入在 houdini 环境只能回落 pointerId=0，
// 会和玩家手指抢同一条 pointer；改成往 /dev/uinput 写一根独立 slot 的手指，
// 内核层直接成一个新触点，互不干扰。
bool AimTouchReady();                        // 确保 uinput 设备存在（懒创建）
bool AimTouchDown(float sx, float sy);       // surface 像素
bool AimTouchMove(float sx, float sy);
bool AimTouchUp();
bool AimTouchActive();                       // 虚拟手指当前是否按住

}  // namespace EvdevTouch
