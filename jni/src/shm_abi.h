// shm_abi.h —— 数据通道 ABI（native 与外部写入端共用，布局变更必须同步改 JS/frida 端）
// 文件协议：/data/data/<pkg>/files/esp_data.bin（或 /sdcard/Android/data/<pkg>/files/esp_data.bin）
//   struct ShmHeader { magic, version, seq(写计数), view(相机), count, ents[N] }
// 写入端用 "写 seq+1（奇数）→ 写数据 → 写 seq+2（偶数）"，读取端校验 seq 为偶数且写前后一致。
#pragma once
#include <cstdint>
#include <cstddef>
#include "vecmath.h"
#include "sensor.h"

#define ESP_SHM_MAGIC   0x45535031u   // "ESP1"
#define ESP_SHM_VERSION 5             // v5：+ camp/aiLevel/liveState（读端按版本算 stride，兼容 v4/v3）
#define ESP_SHM_MAX_ENTS 64

struct ShmEntity {
    uint64_t id;
    float head[3], chest[3], pelvis[3], feet[3], velocity[3];
    float hp, maxHp, distance;
    uint32_t flags;      // bit0 visible, bit1 team, bit2 dead, bit3 screen(屏幕坐标已算好)
    char  name[32];
    uint8_t hasBones;
    uint8_t screenSpace; // v4：1 = 头部三个屏幕坐标有效（JS 侧 worldToScreen 结果）
    uint8_t pad[2];
    float bones[BoneIdx::Count][3];
    // ---- v4 追加：屏幕坐标（0..1 归一化，screenSpace=1 时有效）。读端乘视口尺寸即得像素，
    //      与 surface / canvas 分辨率无关。布局追加在末尾，前面偏移不变。
    float sHead[2], sChest[2], sFeet[2];
    // ---- v4 追加：子弹追踪预解算（JS 侧算好提前量/下坠后写进来，C++ 只管画）
    //      hasLead 存在 flags bit4；sLead 也是 0..1 归一化屏幕坐标
    float sLead[2];
    float leadMeters;    // 提前量（米）
    float dropMeters;    // 下坠量（米）
    float flightTime;    // 预测飞行时间（秒）
    // ---- v5 追加：队营/AI 等级/生活状态（JS 侧从 CharacterCom.data 原样透传，读端据此过滤）
    //      camp: 队伍编号（与本地玩家同 camp 即队友）；aiLevel: >0 为 AI/BOT；
    //      liveState: 0=活 1=死 2=倒地；-1 表示写入端未提供（v3/v4 文件）
    int32_t camp;
    int32_t aiLevel;
    int32_t liveState;
};

// 按版本计算实体步长（读端用，避免旧版文件被新结构体索引导致错位）
#define ESP_SHM_ENT_SZ_V5 ((int)sizeof(ShmEntity))
#define ESP_SHM_ENT_SZ_V4 ((int)sizeof(ShmEntity) - 12)   // v4/v3 无 camp/aiLevel/liveState

struct ShmView {
    float viewProj[16];
    float camPos[3];
    float fovY;
    int32_t viewW, viewH;
};

struct ShmHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t seq;
    ShmView  view;
    int32_t  count;
    int32_t  pad;
    ShmEntity ents[ESP_SHM_MAX_ENTS];
};

// 完整头尺寸 / 最小可读头尺寸（v4 文件比 v5 短 12*64 字节，读端按此下限校验）
#define ESP_SHM_HDR_SZ    ((int)sizeof(ShmHeader))
#define ESP_SHM_HDR_MIN   ((int)(offsetof(ShmHeader, ents) + ESP_SHM_ENT_SZ_V4))

#define ESP_SHM_FLAG_VISIBLE 1u
#define ESP_SHM_FLAG_TEAM    2u
#define ESP_SHM_FLAG_DEAD    4u
#define ESP_SHM_FLAG_SCREEN  8u   // v4：写入端已做 worldToScreen，读端直接用 sHead/sChest/sFeet
#define ESP_SHM_FLAG_LEAD    16u  // v4：写入端已算好提前量/下坠/命中点屏幕坐标（sLead/leadMeters/...）

// ---- ABI 静态断言：任何字段增删或对齐变化都会在这里编译期炸掉，避免 JS 侧 stride 悄悄漂移 ----
// 注意 sizeof 含对齐填充（id 之后的 4B、结构体尾部的 4B），不要用字段长度累加推算。
static_assert(sizeof(ShmView) == 88, "ShmView ABI changed");
static_assert(sizeof(ShmEntity) == 360, "ShmEntity ABI v5 changed (JS ENT_SZ must match)");
static_assert(ESP_SHM_ENT_SZ_V4 == 348, "ShmEntity ABI v4 changed");
static_assert(offsetof(ShmEntity, head) == 8, "ShmEntity::head offset");
static_assert(offsetof(ShmEntity, flags) == 80, "ShmEntity::flags offset");
static_assert(offsetof(ShmEntity, bones) == 120, "ShmEntity::bones offset");
static_assert(offsetof(ShmEntity, sHead) == 300, "ShmEntity::sHead offset");
static_assert(offsetof(ShmEntity, camp) == 344, "ShmEntity::camp offset");
static_assert(offsetof(ShmEntity, liveState) == 352, "ShmEntity::liveState offset");
static_assert(offsetof(ShmHeader, view) == 16, "ShmHeader::view offset");
static_assert(offsetof(ShmHeader, ents) == 112, "ShmHeader::ents offset");
static_assert(sizeof(ShmHeader) == 23152, "ShmHeader ABI changed (JS HDR_SZ must match)");
static_assert(ESP_SHM_HDR_MIN == 460, "ESP_SHM_HDR_MIN changed");
