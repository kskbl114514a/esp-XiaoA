// java_bridge.h —— 通过 JNI 反射把触摸事件直接投递进 Android View 树（自瞄执行器）
#pragma once
#include <jni.h>

namespace JavaBridge {

// 在拿到 JNIEnv 的线程调用一次；ctx 传 Activity 或 Context 都可以（传 null 则自动反射找当前 Activity）
bool Init(JNIEnv* env, jobject ctx);

// 拖拽（模拟手指按住屏幕拖动 → 游戏转视角）
bool BeginDrag(float x, float y);
bool MoveDrag(float x, float y);
bool EndDrag();
bool Dragging();

// 直接投递一次移动（内部自动管理 down/up 生命周期）
//   dx,dy 为「本帧希望转动的像素量」，内部换算为手指从屏幕中心的偏移
bool Nudge(float dx, float dy);

const char* LastError();

}  // namespace JavaBridge
