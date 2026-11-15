// aimbot.h
#pragma once
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
