// hook.h —— 极简 inline hook 封装
//   arm64 : 复用 And64InlineHook（成熟的 A64 trampoline 实现）
//   x86_64: 自带长度解码 + rel32/abs64 跳板（够 hook 库函数入口）
#pragma once
#include <cstdint>
#include <cstddef>

// 安装 hook：把 target 开头改成跳到 hookFn，原函数进入 *origFn。
// 成功返回 true。thread-safe 之外注意：务必在目标函数所在的同一线程上下文调用
// （eglSwapBuffers 在渲染线程 hook，首次 hook 时即完成）。
bool InstallHook(void* target, void* hookFn, void** origFn);

// 把 native 地址改回原来的字节（卸载用）
bool UninstallHook(void* target);

// 取模块基址（/proc/self/maps 中第一个该模块的可执行段起点）
uintptr_t GetModuleBase(const char* soname);

// 取模块大小
size_t GetModuleSize(const char* soname);

// 在模块可执行段里按 AOB 扫描；pattern 例："1F 20 03 D5 ?? ?? ?? ??"
uintptr_t ScanPattern(const char* soname, const char* pattern, int maxHits, uintptr_t* outHits);

// 地址是否处于可执行映射
bool IsExecutableAddr(uintptr_t addr);
