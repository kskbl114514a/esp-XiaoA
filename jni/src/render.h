// render.h —— ImGui 覆盖层生命周期（在 eglSwapBuffers 里驱动）
#pragma once
#include <GLES3/gl3.h>

namespace Render {

// 必须在持有 GL 上下文的线程（渲染线程）首次调用
bool Init();
void Shutdown();

// 关闭 ImGui 全部输入/渲染（hook 卸载时调用）
void SetEnabled(bool on);
bool Enabled();

// 每帧：先 NewFrame，再画菜单/ESP，最后 EndFrame（内含 ImGui::Render + 实际提交）
void NewFrame(int surfaceW, int surfaceH);
void EndFrame();

// 供外部（触摸 hook）向 ImGui 注入输入
struct TouchEvent { int action; float x, y; };
void PushTouch(const TouchEvent& ev);
void InjectRawTouch(int pointerId, int action, float x, float y);

// UI 缩放（按 surface 宽度归一，1080p=1.0）
float UiScale();

// 字体状态（中文是否可用）
const char* FontPath();
bool FontHasCjk();

}
