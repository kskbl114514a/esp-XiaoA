// aimbot.h
#pragma once
// 本文件是【备用】自瞄实现：走 Java MotionEvent 注入(java_bridge)落地。
// 当前线上主用 TouchAim(uinput 独立 slot 虚拟手指)，见 touchaim.cpp。
// 保留原因：某些 ROM 屏蔽 /dev/uinput 时可切回这条通道。
#include "sensor.h"

namespace Aim {

// 每帧更新：选目标、算角度、驱动触摸注入。dt 单位秒。
void Update(const ViewInfo& v, std::vector<Entity>& ents, float dt);
void Reset();

Entity* CurrentTarget();
bool    HasTarget();

// 供菜单显示
float LastDeltaPx();

}  // namespace Aim
