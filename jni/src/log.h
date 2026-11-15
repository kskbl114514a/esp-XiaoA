// log.h —— android logcat 封装，tag 固定 esp
#pragma once
#include <android/log.h>

#define ESP_TAG "esp"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, ESP_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, ESP_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ESP_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, ESP_TAG, __VA_ARGS__)
