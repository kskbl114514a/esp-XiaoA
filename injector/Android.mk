LOCAL_PATH := $(call my-dir)

# ---------------------------------------------------------------------------
# inj_a64 —— arm64-v8a 真机 ptrace 注入器（可执行文件，非 so）
#   产物: libs/arm64-v8a/inj_a64
#   仅在 arm64-v8a ABI 下构建；injector_arm64.c 依赖内核 arm64 pt_regs 布局
# ---------------------------------------------------------------------------
ifeq ($(TARGET_ARCH_ABI),arm64-v8a)
include $(CLEAR_VARS)
LOCAL_MODULE        := inj_a64
LOCAL_SRC_FILES     := injector_arm64.c
LOCAL_LDLIBS        := -llog -ldl
LOCAL_CFLAGS        := -O2 -fPIE -Wall -Wno-unused-result
LOCAL_LDFLAGS       := -pie
include $(BUILD_EXECUTABLE)
endif

# ---------------------------------------------------------------------------
# inj_x64 —— x86_64 注入器（模拟器 / houdini 混血环境）
#   产物: libs/x86_64/inj_x64
#   injector_x64.c 依赖 x86_64 的 struct user_regs_struct，只在 x86_64 ABI 下构建
# ---------------------------------------------------------------------------
ifeq ($(TARGET_ARCH_ABI),x86_64)
include $(CLEAR_VARS)
LOCAL_MODULE        := inj_x64
LOCAL_SRC_FILES     := injector_x64.c
LOCAL_LDLIBS        := -llog -ldl
LOCAL_CFLAGS        := -O2 -fPIE -Wall -Wno-unused-result
LOCAL_LDFLAGS       := -pie
include $(BUILD_EXECUTABLE)
endif
