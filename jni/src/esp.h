// esp.h —— 透视绘制
#pragma once
#include "imgui.h"
#include "sensor.h"

namespace Esp {
void Draw(ImDrawList* dl, const ViewInfo& view, std::vector<Entity>& ents);
void DrawWatermark(ImDrawList* dl, float w, float h, int entCount, const char* sensor, float ms);
}
