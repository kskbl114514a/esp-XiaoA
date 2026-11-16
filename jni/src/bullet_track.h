// bullet_track.h —— 子弹追踪（替代自瞄）：只做弹道解算与可视化，不驱动转向
#pragma once
#include "sensor.h"

struct ImDrawList;

namespace Track {

struct Hit {
    bool   valid      = false;
    int    entIndex   = -1;
    Vec3   point;              // 预测命中点（世界坐标，含提前量+下坠补偿）
    Vec3   leadVec;            // 提前量矢量
    float  leadMeters = 0.f;   // 提前量模长
    float  dropMeters = 0.f;   // 下坠补偿量
    float  dist       = 0.f;   // 相机 → 命中点
    float  flightTime = 0.f;   // 弹丸飞行时间（秒）
    Vec2   sPoint;             // 预测点屏幕坐标
    Vec2   sNow;               // 目标当前屏幕坐标
    bool   onScreen   = false;
    bool   inFov      = true;
    bool   useMatrix  = true;   // 本次解算是否走矩阵（false=JS 已给屏幕坐标，弹道弧无法画）
};

// 每帧：选目标 → 解算弹道 → 填 g_hit（不产生任何输入事件）
void Update(const ViewInfo& v, std::vector<Entity>& ents, float dt);

// 绘制预测点/弹道弧/信息文本
void Draw(ImDrawList* dl, const ViewInfo& v);

void  Reset();
const Hit& Current();
int   CandidateCount();
float LastSolveMs();

}  // namespace Track
