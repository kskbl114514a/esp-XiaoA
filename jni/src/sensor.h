// sensor.h —— 数据源抽象。ESP/自瞄只认识 Entity，不认识 Cocos。
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include "vecmath.h"

struct BoneIdx {
    enum { Head = 0, Neck, Chest, Spine, Pelvis, LShoulder, RShoulder, LElbow, RElbow,
           LHip, RHip, LKnee, RKnee, LFoot, RFoot, Count };
};

struct Entity {
    uint64_t id = 0;
    Vec3  head;         // 世界坐标（米）
    Vec3  chest;
    Vec3  pelvis;
    Vec3  feet;
    Vec3  velocity;     // 用于预判
    float hp = 100.f, maxHp = 100.f;
    float distance = 0.f;    // 到相机（米）
    bool  visible = true;    // 未被遮挡
    bool  isTeam = false;
    bool  isDead = false;
    char  name[40] = {0};
    bool  hasBones = false;
    Vec3  bones[BoneIdx::Count];

    // v4：写入端（JS）已在游戏内做过 worldToScreen，屏幕坐标直接可用，跳过矩阵投影
    //      （值是 0..1 归一化比例，渲染层乘视口宽高得像素）
    bool  screenSpace = false;
    float screenHeadX = 0.f, screenHeadY = 0.f;
    float screenChestX = 0.f, screenChestY = 0.f;
    float screenFeetX = 0.f, screenFeetY = 0.f;

    // v4：子弹追踪预解算（JS 侧算好，C++ 侧只画；值为 0..1 归一化屏幕坐标）
    bool  hasLead = false;
    float leadScrX = 0.f, leadScrY = 0.f;
    float leadMeters = 0.f, dropMeters = 0.f, flightTime = 0.f;

    // v5：JS 侧透传的原始判定字段（-1 = 写入端未提供，读端不据此过滤）
    int   camp = -1;         // 队营编号，与本地 camp 相同即队友
    int   aiLevel = -1;      // >0 = AI/BOT
    int   liveState = -1;    // 0=活 1=死 2=倒地
    bool  isAI = false;      // aiLevel>0 派生
    bool  isKnocked = false; // liveState==2 派生

    // 每帧计算结果
    Vec2  sHead, sChest, sPelvis, sFeet;
    Vec2  sBones[BoneIdx::Count];
    bool  onScreen = false;
};

struct ViewInfo {
    bool  valid = false;
    Mat4  viewProj;         // 视图投影矩阵（已与渲染视口匹配）
    Vec3  camPos;
    float fovY = 60.f;      // 垂直 FOV（度）
    int   viewW = 0, viewH = 0;   // 渲染视口像素尺寸（与屏幕一致时等于 surface）
    double frameTime = 0;
};

class ISensor {
public:
    virtual ~ISensor() {}
    virtual const char* Name() const = 0;
    virtual bool Init() = 0;
    virtual bool Update(ViewInfo& view, std::vector<Entity>& out) = 0;
    virtual void Dump() {}       // 标定期的场景 dump
};

ISensor* CreateShmSensor();      // 外部进程/脚本通过共享内存喂数据（标定期 & frida 通道）
ISensor* CreateNativeSensor();   // 直接遍历 Cocos 场景图（需要偏移标定）

// 由传感器选择器按 g_cfg.sensorMode 取实例
ISensor* GetSensor();
void     ResetSensor();
// 探测 JS 写入的 esp_data.bin 是否已有有效数据（auto 模式切数据源用）
bool     ProbeShmOnce();
