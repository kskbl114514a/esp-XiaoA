// java_bridge.cpp
// 说明：自瞄不走「改相机 yaw/pitch」（那样需要标定 Camera 结构，且很多游戏的相机每帧被逻辑重写），
// 走「注入触摸拖动」：游戏把手指滑动映射成视角旋转，注入等同真实手指，版本无关、通用。
#include "java_bridge.h"
#include <pthread.h>
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

// 附着到 VM 的线程必须在退出前 DetachCurrentThread，否则 VM 认为线程仍存在，
// 反复 hook 卸载/重载会堆积 JNI 附着（日志刷 "thread not detached"，极端情况 abort）。
// 这里用 pthread_key 的析构函数兜底：谁 Attach 的，线程一退出就自动 Detach。
static pthread_key_t  g_tlKey;
static pthread_once_t g_tlOnce = PTHREAD_ONCE_INIT;
static void TlDetach(void*) {
    if (!g_vm) return;
    JNIEnv* e = nullptr;
    if (g_vm->GetEnv((void**)&e, JNI_VERSION_1_6) == JNI_OK) g_vm->DetachCurrentThread();
}
static void TlInit() { pthread_key_create(&g_tlKey, TlDetach); }

static JNIEnv* Env() {
    if (!g_vm) return nullptr;
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
        pthread_once(&g_tlOnce, TlInit);
        pthread_setspecific(g_tlKey, (void*)1);   // 绑定析构，线程退出自动 Detach
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
    if (!g_dispatch) {
        env->ExceptionClear(); SetErr("找不到 dispatchTouchEvent");
        env->DeleteLocalRef(actCls); env->DeleteLocalRef(act);   // 失败路径也要还局部引用
        return false;
    }

    jclass meCls = env->FindClass("android/view/MotionEvent");
    if (!meCls) {
        env->ExceptionClear(); SetErr("找不到 MotionEvent");
        env->DeleteLocalRef(actCls); env->DeleteLocalRef(act);
        return false;
    }
    g_obtain = env->GetStaticMethodID(meCls, "obtain",
        "(JJIFFI)Landroid/view/MotionEvent;");
    if (!g_obtain) {
        env->ExceptionClear(); SetErr("找不到 MotionEvent.obtain");
        env->DeleteLocalRef(meCls); env->DeleteLocalRef(actCls); env->DeleteLocalRef(act);
        return false;
    }
    g_recycle = env->GetMethodID(meCls, "recycle", "()V");
    g_setSource = env->GetMethodID(meCls, "setSource", "(I)V");

    // ---- 多点注入通道：自定义 pointerId，避免与玩家真实手指（id 0）抢同一条 pointer
    // 这条路径能用时，自瞄手指与开火手指可同时存在，不会互相打断。
    do {
        // 原来直接把 FindClass 的返回值存进成员：jclass 是【局部引用】，
        // 出了 Init 就失效，之后在别的线程/别的帧用 g_ppCls 去 GetFieldID 属于
        // 使用悬垂 JNI 引用（未定义行为，实测表现为注入线程偶发崩在 Obtain）。
        // 必须 NewGlobalRef 提升为全局引用。
        jclass ppLocal = env->FindClass("android/view/MotionEvent$PointerProperties");
        jclass pcLocal = env->FindClass("android/view/MotionEvent$PointerCoords");
        if (!ppLocal || !pcLocal) {
            env->ExceptionClear();
            if (ppLocal) env->DeleteLocalRef(ppLocal);
            if (pcLocal) env->DeleteLocalRef(pcLocal);
            break;
        }
        g_ppCls = (jclass)env->NewGlobalRef(ppLocal);
        g_pcCls = (jclass)env->NewGlobalRef(pcLocal);
        env->DeleteLocalRef(ppLocal);
        env->DeleteLocalRef(pcLocal);
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
        if (!pp || !pc) {
            env->ExceptionClear();
            if (pp) env->DeleteLocalRef(pp);
            if (pc) env->DeleteLocalRef(pc);
            break;
        }
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

// 返回是否真的把事件塞进了 Activity：原来 void，调用方无法感知失败，
// 会在"事件其实没进去"的情况下继续推进内部坐标，越推越偏。
static bool Dispatch(int action, float x, float y) {
    JNIEnv* env = Env();
    if (!env || !g_activity) { SetErr("触摸注入未初始化"); return false; }
    jobject ev = Obtain(env, action, x, y);
    if (!ev) { SetErr("MotionEvent.obtain 失败"); return false; }
    jboolean consumed = env->CallBooleanMethod(g_activity, g_dispatch, ev);
    bool ok = true;
    if (env->ExceptionCheck()) { env->ExceptionClear(); SetErr("dispatchTouchEvent 抛异常"); ok = false; }
    (void)consumed;   // 返回 false 说明 Activity 没消费，但事件已送达，仍算投递成功
    if (g_recycle) env->CallVoidMethod(ev, g_recycle);
    env->DeleteLocalRef(ev);
    return ok;
}

bool Dragging() { return g_dragging; }

// 菜单拖动标记：菜单 BeginDrag 与自瞄 Nudge 共用同一根虚拟手指（g_px/g_py/g_dragging），
// 不隔离的话菜单一拖，自瞄每帧把坐标改回去，表现为菜单飘/自瞄抽搐。
static bool g_uiDrag = false;

bool BeginDrag(float x, float y) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_dragging) return true;
    g_anchorX = x; g_anchorY = y;
    g_px = x; g_py = y;
    g_downTime = NowMs();
    if (!Dispatch(0 /*ACTION_DOWN*/, x, y)) return false;
    g_dragging = true;
    g_uiDrag = true;
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
    g_uiDrag = false;
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
    if (g_uiDrag) return false;              // 菜单正在被拖，这一帧不许抢手指
    if (!g_dragging) {
        g_anchorX = ax; g_anchorY = ay;
        g_downTime = NowMs();
        g_px = ax; g_py = ay;
        if (!Dispatch(0, g_px, g_py)) return false;   // ACTION_DOWN 没进去就别装已按下
        g_dragging = true;
    }
    // 手指漂出安全区就抬起重按，长时间按住会被某些游戏当成"长按"处理
    if (fabsf(g_px - ax) > limit * 1.5f || fabsf(g_py - ay) > limit * 1.5f) {
        Dispatch(1, g_px, g_py);           // ACTION_UP
        g_downTime = NowMs();
        g_px = ax; g_py = ay;
        if (!Dispatch(0, g_px, g_py)) { g_dragging = false; return false; }  // 重按失败要清状态
    }
    const float nx = g_px + dx, ny = g_py + dy;
    if (!Dispatch(2, nx, ny)) return false;            // 投递失败不推进内部坐标
    g_px = nx; g_py = ny;                              // ACTION_MOVE
    return true;
}

}  // namespace JavaBridge
