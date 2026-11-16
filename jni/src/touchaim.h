// touchaim.h —— 触摸自瞄：选目标 + 闭环拖动投递
#pragma once
#include "sensor.h"
#include <vector>

namespace TouchAim {

// 每帧调用（放在 Esp::Draw 之后、Menu::Draw 之前）。dt 单位秒。
void Update(const ViewInfo& v, std::vector<Entity>& ents, float dt);

// 立刻松开虚拟手指并清状态（关菜单/退出时调用）
void Reset();

Entity* CurrentTarget();
bool    HasTarget();
float   LastDeltaPx();

// 菜单展示用
const char* StateText();      // 空闲 / 拖动 / 回中 / 无通道
int         MoveFrames();     // 已投递的移动帧数
const char* LastError();      // 投递失败原因
bool        ChannelOk();      // uinput 虚拟手指是否可用

}  // namespace TouchAim
