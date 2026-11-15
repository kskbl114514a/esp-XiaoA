// menu.h
#pragma once

namespace Menu {
// 绘制主菜单 / 收起时的唤起按钮 / 实时参数面板
// 返回是否处于展开状态
bool Draw(int surfaceW, int surfaceH);

void Load();
void Save();
void ApplyPreset(int idx);
}
