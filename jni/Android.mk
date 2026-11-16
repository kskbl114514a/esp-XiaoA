LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE    := esp
LOCAL_SRC_FILES := \
    src/entry.cpp \
    src/render.cpp \
    src/esp.cpp \
    src/menu.cpp \
    src/sensor.cpp \
    src/sensor_native.cpp \
    src/bullet_track.cpp \
    src/hook.cpp \
    src/touch.cpp \
    src/evtouch.cpp \
    src/touchaim.cpp \
    src/aimbot.cpp \
    src/java_bridge.cpp \
    third_party/And64InlineHook.cpp \
    imgui/imgui.cpp \
    imgui/imgui_draw.cpp \
    imgui/imgui_tables.cpp \
    imgui/imgui_widgets.cpp \
    imgui/imgui_impl_opengl3.cpp

LOCAL_C_INCLUDES := $(LOCAL_PATH)/src $(LOCAL_PATH)/imgui $(LOCAL_PATH)/third_party
LOCAL_CPPFLAGS   := -std=c++17 -O2 -fPIC -ffunction-sections -fdata-sections \
                    -DIMGUI_IMPL_OPENGL_ES3=1 -DIMGUI_DEFINE_MATH_OPERATORS=1 \
                    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
                    -Wno-sign-compare -Wno-missing-field-initializers -Wno-writable-strings
LOCAL_CFLAGS     := -O2 -fPIC
LOCAL_LDFLAGS    := -Wl,--gc-sections
LOCAL_LDLIBS     := -llog -landroid -lEGL -lGLESv3 -lGLESv2 -ldl -lm

include $(BUILD_SHARED_LIBRARY)