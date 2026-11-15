// touch.h —— 触摸接管（几何命中 与 拦截决策 分离）
// 设计要点：
//   InMenuRect    = 纯几何命中（点是否落在菜单窗口上），与"吞不吞"无关 → 决定事件要不要喂给 ImGui
//   ShouldConsume = 拦截决策（是否把事件从游戏手里截走）→ 只由 menuCaptureTouch + evdev Grab 决定
// 这样「不吞触摸」时 ImGui 依然收得到事件，菜单照样点得动。
#pragma once

namespace TouchHook {

// 在 game 的 libcocos 已加载后调用
bool Install();
void Uninstall();

// 当前是否有「开火侧」按压（右半屏按下）
bool IsFireHeld();

// 菜单窗口矩形（由渲染线程每帧更新给触摸线程用于命中判定）
struct Rect { float x, y, w, h; };
void SetMenuRect(const Rect& r, bool open);
void SetToggleRect(const Rect& r);
Rect GetMenuRect();
bool MenuOpenByTouch();
void SetMenuOpen(bool open);

// 几何命中（不含拦截判断）——evdev 用它决定"这根手指归 ImGui"
bool InMenuRect(float x, float y);
bool HitToggleRect(float x, float y);

// 拦截决策（= 菜单展开 && menuCaptureTouch && 几何命中）
bool HitMenuRect(float x, float y);
bool MenuCapture();

}  // namespace TouchHook
