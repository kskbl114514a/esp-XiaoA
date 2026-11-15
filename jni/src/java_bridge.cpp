// java_bridge.cpp
// 说明：自瞄不走「改相机 yaw/pitch」（那样需要标定 Camera 结构，且很多游戏的相机每帧被逻辑重写），
// 走「注入触摸拖动」：游戏把手指滑动映射成视角旋转，注入等同真实手指，版本无关、通用。
#include "java_bridge.h"
#include "log.h"
#include "config.h"
#include "vecmath.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <time.h>

namespace JavaBridge {

static JavaVM*  g_vm = nullptr;
static jobject  g_activity = nullptr;     // GlobalRef
static jclass  g_meCls    = nullptr;   // MotionEvent 类全局引用
static jmethodID g_dispatch = nullptr;
static jmethodID g_obtain = nullptr;
static jmethodID g_obtainMulti = nullptr;   // 带 PointerProperties/PointerCoords 的版本
static jmethodID g_recycle = nullptr;
static jmethodID g_setSource = nullptr;
static jmethodID g_getSource = nullptr;

// 多点注入用的持久对象（避免 120Hz 下每帧 new 一坨 Java 对象触发 GC）
static jclass   g_ppCls = nullptr, g_pcCls = nullptr;
static jobject  g_pp = nullptr, g_pc = nullptr;       // GlobalRef，单例属性对象
static jobjectArray g_ppArr = nullptr, g_pcArr = nullptr;
static jfieldID g_fPid = nullptr, g_fTool = nullptr;
static jfieldID g_fX = nullptr, g_fY = nullptr, g_fP1 = nullptr, g_fS1 = nullptr;

static bool  g_dragging = false;
static float g_px = 0, g_py = 0;          // 逻辑手指位置
static float g_anchorX = 0, g_anchorY = 0;
static int64_t g_downTime = 0;
static char  g_err[192] = {0};
static std::mutex g_mtx;

static int64_t NowMs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void SetErr(const char* what) {
    snprintf(g_err, sizeof(g_err), "%s", what ? what : "");
    LOGE("%s", g_err);
}

const char* LastError() { return g_err; }

static JNIEnv* Env() {
    if (!g_vm) return nullptr;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    }
    return env;
}

static jobject FindCurrentActivity(JNIEnv* env) {
    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (!atCls) { env->ExceptionClear(); return nullptr; }
    jmethodID cur = env->GetStaticMethodID(atCls, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if (!cur) { env->ExceptionClear(); return nullptr; }
    jobject at = env->CallStaticObjectMethod(atCls, cur);
    if (!at) { env->ExceptionClear(); return nullptr; }

    jfieldID fAct = env->GetFieldID(atCls, "mActivities", "Landroid/util/ArrayMap;");
    if (!fAct) { env->ExceptionClear(); return nullptr; }
    jobject map = env->GetObjectField(at, fAct);
    if (!map) { env->ExceptionClear(); return nullptr; }

    jclass mapCls = env->GetObjectClass(map);
    jmethodID sizeM = env->GetMethodID(mapCls, "size", "()I");
    jmethodID valueAt = env->GetMethodID(mapCls, "valueAt", "(I)Ljava/lang/Object;");
    jmethodID keyAt = env->GetMethodID(mapCls, "keyAt", "(I)Ljava/lang/Object;");
    jobject result = nullptr;
    if (sizeM && valueAt) {
        jint n = env->CallIntMethod(map, sizeM);
        for (jint i = 0; i < n && !result; ++i) {
            jobject rec = env->CallObjectMethod(map, valueAt, i);
            if (!rec) continue;
            jclass recCls = env->GetObjectClass(rec);
            jfieldID fA = env->GetFieldID(recCls, "activity", "Landroid/app/Activity;");
            if (fA) {
                jobject act = env->GetObjectField(rec, fA);
                if (act) {
                    // paused 的不要
                    jfieldID fPaused = env->GetFieldID(recCls, "paused", "Z");
                    bool paused = false;
                    if (fPaused) paused = env->GetBooleanField(rec, fPaused) == JNI_TRUE;
                    (void)keyAt;
                    if (!paused) result = act;
                    else env->DeleteLocalRef(act);
                }
            }
            env->ExceptionClear();
            env->DeleteLocalRef(recCls);
            env->DeleteLocalRef(rec);
        }
    }
    env->ExceptionClear();
    env->DeleteLocalRef(mapCls);
    env->DeleteLocalRef(map);
    env->DeleteLocalRef(at);
    env->DeleteLocalRef(atCls);
    return result;
}

bool Init(JNIEnv* env, jobject ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_activity) return true;
    if (!env) return false;
    env->GetJavaVM(&g_vm);

    jobject act = nullptr;
    if (ctx) {
        // 若传进来的是 Context，转成 Activity 不一定成立；直接尝试并留兼容
        act = env->NewLocalRef(ctx);
    } else {
        act = FindCurrentActivity(env);
    }
    if (!act) { SetErr("未找到 Activity（自瞄触摸注入不可用，ESP 不受影响）"); return false; }

    jclass actCls = env->GetObjectClass(act);
    g_dispatch = env->GetMethodID(actCls, "dispatchTouchEvent", "(Landroid/view/MotionEvent;)Z");
    if (!g_dispatch) { env->ExceptionClear(); SetErr("找不到 dispatchTouchEvent"); return false; }

    jclass meCls = env->FindClass("android/view/MotionEvent");
    if (!meCls) { env->ExceptionClear(); SetErr("找不到 MotionEvent"); return false; }
    g_obtain = env->GetStaticMethodID(meCls, "obtain",
        "(JJIFFI)Landroid/view/MotionEvent;");
    if (!g_obtain) { env->ExceptionClear(); SetErr("找不到 MotionEvent.obtain"); return false; }
    g_recycle = env->GetMethodID(meCls, "recycle", "()V");
    g_setSource = env->GetMethodID(meCls, "setSource", "(I)V");

    // ---- 多点注入通道：自定义 pointerId，避免与玩家真实手指（id 0）抢同一条 pointer
    // 这条路径能用时，自瞄手指与开火手指可同时存在，不会互相打断。
    do {
        g_ppCls = env->FindClass("android/view/MotionEvent$PointerProperties");
        g_pcCls = env->FindClass("android/view/MotionEvent$PointerCoords");
        if (!g_ppCls || !g_pcCls) { env->ExceptionClear(); break; }

        jmethodID ppInit = env->GetMethodID(g_ppCls, "<init>", "()V");
        jmethodID pcInit = env->GetMethodID(g_pcCls, "<init>", "()V");
        g_fPid  = env->GetFieldID(g_ppCls, "id", "I");
        g_fTool = env->GetFieldID(g_ppCls, "toolType", "I");
        g_fX    = env->GetFieldID(g_pcCls, "x", "F");
        g_fY    = env->GetFieldID(g_pcCls, "y", "F");
        g_fP1   = env->GetFieldID(g_pcCls, "pressure", "F");
        g_fS1   = env->GetFieldID(g_pcCls, "size", "F");
        g_obtainMulti = env->GetStaticMethodID(meCls, "obtain",
            "(JJII[Landroid/view/MotionEvent$PointerProperties;"
            "[Landroid/view/MotionEvent$PointerCoords;IIFFII)Landroid/view/MotionEvent;");
        if (!ppInit || !pcInit || !g_fPid || !g_fX || !g_fY || !g_obtainMulti) {
            env->ExceptionClear(); g_obtainMulti = nullptr; break;
        }
        jobject pp = env->NewObject(g_ppCls, ppInit);
        jobject pc = env->NewObject(g_pcCls, pcInit);
        if (!pp || !pc) { env->ExceptionClear(); break; }
        g_pp = env->NewGlobalRef(pp);
        g_pc = env->NewGlobalRef(pc);
        env->DeleteLocalRef(pp);
        env->DeleteLocalRef(pc);
        g_ppArr = (jobjectArray)env->NewGlobalRef(env->NewObjectArray(1, g_ppCls, g_pp));
        g_pcArr = (jobjectArray)env->NewGlobalRef(env->NewObjectArray(1, g_pcCls, g_pc));
        LOGI("多点注入通道就绪 pointerId=%d", g_cfg.aimPointerId);
    } while (0);
    if (!g_obtainMulti) LOGW("多点注入不可用，回落 pointerId=0 单点注入（会与玩家手指互抢）");

    g_activity = env->NewGlobalRef(act);
    g_meCls = (jclass)env->NewGlobalRef(meCls);
    env->DeleteLocalRef(meCls);
    env->DeleteLocalRef(actCls);
    env->DeleteLocalRef(act);
    LOGI("触摸注入就绪 activity=%p", g_activity);
    return true;
}

static jobject Obtain(JNIEnv* env, int action, float x, float y) {
    if (!g_meCls) {
        jclass c = env->FindClass("android/view/MotionEvent");
        if (!c) { env->ExceptionClear(); return nullptr; }
        g_meCls = (jclass)env->NewGlobalRef(c);
        env->DeleteLocalRef(c);
    }
    int64_t now = NowMs();

    // 优先走多点通道：pointerId 固定为 aimPointerId，和玩家真实手指隔离
    if (g_obtainMulti && g_pp && g_pc && g_ppArr && g_pcArr) {
        env->SetIntField(g_pp, g_fPid, (jint)g_cfg.aimPointerId);
        if (g_fTool) env->SetIntField(g_pp, g_fTool, (jint)1);   // TOOL_TYPE_FINGER
        env->SetFloatField(g_pc, g_fX, (jfloat)x);
        env->SetFloatField(g_pc, g_fY, (jfloat)y);
        if (g_fP1) env->SetFloatField(g_pc, g_fP1, 1.0f);
        if (g_fS1) env->SetFloatField(g_pc, g_fS1, 1.0f);
        jobject o = env->CallStaticObjectMethod(g_meCls, g_obtainMulti,
            (jlong)g_downTime, (jlong)now, (jint)action, (jint)1,
            g_ppArr, g_pcArr, (jint)0, (jint)0, 1.0f, 1.0f, (jint)0, (jint)0);
        if (env->ExceptionCheck()) { env->ExceptionClear(); o = nullptr; }
        if (o) {
            if (g_setSource) {
                env->CallVoidMethod(o, g_setSource, (jint)0x00001002 /* SOURCE_TOUCHSCREEN */);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            return o;
        }
        // 多点失败则本帧回落单点
    }

    jobject o = env->CallStaticObjectMethod(g_meCls, g_obtain, (jlong)g_downTime, (jlong)now,
                                            (jint)action, (jfloat)x, (jfloat)y, (jint)0);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (o && g_setSource) {
        env->CallVoidMethod(o, g_setSource, (jint)0x00001002 /* SOURCE_TOUCHSCREEN */);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    return o;
}

static void Dispatch(int action, float x, float y) {
    JNIEnv* env = Env();
    if (!env || !g_activity) { SetErr("触摸注入未初始化"); return; }
    jobject ev = Obtain(env, action, x, y);
    if (!ev) { SetErr("MotionEvent.obtain 失败"); return; }
    env->CallBooleanMethod(g_activity, g_dispatch, ev);
    if (env->ExceptionCheck()) { env->ExceptionClear(); SetErr("dispatchTouchEvent 抛异常"); }
    if (g_recycle) env->CallVoidMethod(ev, g_recycle);
    env->DeleteLocalRef(ev);
}

bool Dragging() { return g_dragging; }

bool BeginDrag(float x, float y) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_dragging) return true;
    g_anchorX = x; g_anchorY = y;
    g_px = x; g_py = y;
    g_downTime = NowMs();
    Dispatch(0 /*ACTION_DOWN*/, x, y);
    g_dragging = true;
    return true;
}

bool MoveDrag(float x, float y) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_dragging) return false;
    g_px = x; g_py = y;
    Dispatch(2 /*ACTION_MOVE*/, x, y);
    return true;
}

bool EndDrag() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_dragging) return false;
    Dispatch(1 /*ACTION_UP*/, g_px, g_py);
    g_dragging = false;
    return true;
}

// 自瞄核心输出：把本帧要转的像素量转成一次手指位移
// 实现要点：
//   - 手指始终"按住"在屏幕右半区（避免与玩家左拇指摇杆冲突），每次移动后回弹到锚点
//   - 单次位移上限 40 像素，避免一帧怼出巨大 delta 被游戏平滑掉或触发异常检测
bool Nudge(float dx, float dy) {
    if (!g_activity) return false;
    const float sw = (float)g_cfg.screenW, sh = (float)g_cfg.screenH;
    if (sw < 10.f || sh < 10.f) return false;

    const float k = sw / 1080.0f;
    const float limit = (g_cfg.aimMaxStepPx > 1.f ? g_cfg.aimMaxStepPx : 60.f) * k;
    dx = ClampF(dx, -limit, limit);
    dy = ClampF(dy, -limit, limit);
    if (fabsf(dx) < 0.35f && fabsf(dy) < 0.35f) return false;

    const float ax = sw * ClampF(g_cfg.aimAnchorX, 0.20f, 0.90f);
    const float ay = sh * ClampF(g_cfg.aimAnchorY, 0.15f, 0.90f);

    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_dragging) {
        g_anchorX = ax; g_anchorY = ay;
        g_downTime = NowMs();
        g_px = ax; g_py = ay;
        Dispatch(0, g_px, g_py);           // ACTION_DOWN
        g_dragging = true;
    }
    // 手指漂出安全区就抬起重按，长时间按住会被某些游戏当成"长按"处理
    if (fabsf(g_px - ax) > limit * 1.5f || fabsf(g_py - ay) > limit * 1.5f) {
        Dispatch(1, g_px, g_py);           // ACTION_UP
        g_downTime = NowMs();
        g_px = ax; g_py = ay;
        Dispatch(0, g_px, g_py);           // 重新 ACTION_DOWN
    }
    g_px += dx; g_py += dy;
    Dispatch(2, g_px, g_py);               // ACTION_MOVE
    return true;
}

}  // namespace JavaBridge
