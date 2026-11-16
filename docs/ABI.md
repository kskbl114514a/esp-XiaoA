---
AIGC:
    Label: "1"
    ContentProducer: 001191440300708461136T1XGW3
    ProduceID: c7bba7cd687c3be43ccd054e5d679f26_1b9e6555e14d11f1913b525400393706
    ReservedCode1: 2vYt8oXmlmbCMspyeCBWWhvmfIRlneCWaUu3xlNBPFaiDKOw+2hrdEOGy8wPqmF/SnZWC5f9OpbhmQAPnu8wJJ+GUjsBE6aJohBGKQ3ciMrwOushOMo1hgxO71jJlGWD8YS8RONBmTs5S7CPzXtaOGS2z1QAtscj7tt+5mbVOv/ay0VLobyVsj4CkT0=
    ContentPropagator: 001191440300708461136T1XGW3
    PropagateID: c7bba7cd687c3be43ccd054e5d679f26_1b9e6555e14d11f1913b525400393706
    ReservedCode2: 2vYt8oXmlmbCMspyeCBWWhvmfIRlneCWaUu3xlNBPFaiDKOw+2hrdEOGy8wPqmF/SnZWC5f9OpbhmQAPnu8wJJ+GUjsBE6aJohBGKQ3ciMrwOushOMo1hgxO71jJlGWD8YS8RONBmTs5S7CPzXtaOGS2z1QAtscj7tt+5mbVOv/ay0VLobyVsj4CkT0=
---

# 数据通道 ABI（ESP_SHM v5）

`esp_data.bin` 是 JS 写端与 Native 读端之间**唯一的契约**。两端不共享任何运行时代码，只靠这块固定布局的二进制对齐；所以本文档里的每一个偏移、每一个 `sizeof` 都必须与 `jni/src/shm_abi.h` 完全一致。

权威定义：[`jni/src/shm_abi.h`](../jni/src/shm_abi.h)。该头文件末尾有一组 `static_assert`，任何字段增删或对齐变化都会**编译期报错**，这是防止 JS 侧 stride 悄悄漂移的最后一道闸。

---

## 1. 文件位置

| 项 | 值 |
|---|---|
| 路径 | `/data/local/tmp/esp_data.bin`（脚本版）<br>`<writablePath>/esp_data.bin`（JS 写端默认） |
| 写端 | 游戏进程内 JS：`native.fileUtils.writeDataToFile(path, u8)` |
| 读端 | `libesp.so` 的 `sensor.cpp`，每帧 `open + fstat + read` 全量快照 |
| 大小 | v5 写满 64 实体 = **23,152 B**；v4/v3 写满 = 22,480 B |

**读端不用 mmap 长映射**：JS 的 `writeDataToFile` 等价 `fopen("wb")`，会先把文件截断到 0 再写，reader 若持着旧 mmap 访问越界页会直接 `SIGBUS` 崩进程。所以用 `read()` 一次性快照。

---

## 2. 常量

```c
#define ESP_SHM_MAGIC    0x45535031u   // "ESP1"
#define ESP_SHM_VERSION  5
#define ESP_SHM_MAX_ENTS 64
```

## 3. 结构布局

### 3.1 `ShmView` —— 相机（88 B）

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | `float[16]` | `viewProj` | 列主序 VP 矩阵；JS 侧保留字段，当前写 0 矩阵占位 |
| 64 | `float[3]` | `camPos` | 相机世界坐标 |
| 76 | `float` | `fovY` | 垂直 FOV（度） |
| 80 | `int32` | `viewW` | 视口宽（像素） |
| 84 | `int32` | `viewH` | 视口高（像素） |

### 3.2 `ShmEntity` —— 实体（v5 = 360 B / v4·v3 = 348 B）

**注意对齐填充**：`uint64_t id` 后面跟 `float head[3]` 会产生 4 B 尾部填充（所以 `head@8` 不是 `@4`）；`bones` 同理；结构体整体按 8 字节对齐，`356 → 360`，最后这 4 B 也是填充。**绝对不要用"字段长度相加"去推 `sizeof`。**

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | `uint64_t` | `id` | 实体唯一 ID |
| 8 | `float[3]` | `head` | 世界坐标（米） |
| 20 | `float[3]` | `chest` | |
| 32 | `float[3]` | `pelvis` | |
| 44 | `float[3]` | `feet` | |
| 56 | `float[3]` | `velocity` | 用于预判，米/秒 |
| 68 | `float` | `hp` | |
| 72 | `float` | `maxHp` | |
| 76 | `float` | `distance` | 到相机距离（米） |
| 80 | `uint32` | `flags` | 见 §4 |
| 84 | `char[32]` | `name` | UTF-8，不足补 0 |
| 116 | `uint8` | `hasBones` | 骨骼数据有效 |
| 117 | `uint8` | `screenSpace` | 屏幕坐标有效（0..1 归一化） |
| 118 | `uint8[2]` | `pad` | 显式占位，禁止挪用 |
| 120 | `float[15][3]` | `bones` | 见 §5 骨骼索引表 |
| 300 | `float[2]` | `sHead` | 头部屏幕坐标 |
| 308 | `float[2]` | `sChest` | 胸部 |
| 316 | `float[2]` | `sFeet` | 脚部 |
| 324 | `float[2]` | `sLead` | 提前量命中点屏幕坐标（`FLAG_LEAD`） |
| 332 | `float` | `leadMeters` | 提前量位移长度（米，非负） |
| 336 | `float` | `dropMeters` | 下坠量（米） |
| 340 | `float` | `flightTime` | 预测飞行时间（秒） |
| **344** | `int32` | **`camp`** | v5 新增：阵营号，与本机相同即队友；`-1` 未知 |
| **348** | `int32` | **`aiLevel`** | v5 新增：`>0` 即 AI / BOT；`-1` 未知 |
| **352** | `int32` | **`liveState`** | v5 新增：`0` 活 / `1` 死 / `2` 倒地；`-1` 未知 |
| 356 | — | *(4 B 尾部对齐填充)* | **不计入字段，但计入 stride** |
| **360** | | **`sizeof(ShmEntity)`** | |

屏幕坐标 `sHead/sChest/sFeet/sLead` 统一是 `0..1` 归一化值：`屏幕像素 = 归一化值 × 视口尺寸`。与 surface / canvas 分辨率无关，换设备不用改 ABI。

### 3.3 `ShmHeader` —— 头部（23,152 B）

| 偏移 | 类型 | 字段 | 说明 |
|---|---|---|---|
| 0 | `uint32` | `magic` | `0x45535031` |
| 4 | `uint32` | `version` | 当前 5 |
| 8 | `uint64` | `seq` | 写序号（`uint64`，注意 8 字节） |
| 16 | `ShmView` | `view` | 88 B |
| 104 | `int32` | `count` | 有效实体数（≤ 64） |
| 108 | `int32` | `pad` | 占位 |
| **112** | `ShmEntity[64]` | `ents` | `64 × 360 = 23,040` B |

### 3.4 尺寸常量速查

| 宏 | 值 | 含义 |
|---|---|---|
| `sizeof(ShmView)` | 88 | |
| `sizeof(ShmEntity)` / `ESP_SHM_ENT_SZ_V5` | **360** | v5 实体步长 |
| `ESP_SHM_ENT_SZ_V4` | **348** | `sizeof - 12`，v4/v3 实体步长 |
| `sizeof(ShmHeader)` / `ESP_SHM_HDR_SZ` | **23,152** | 满 64 实体的完整头 |
| `ESP_SHM_HDR_MIN` | **460** | `offsetof(ents) + 348`，v4 写出的最小可读长度 |

JS 侧必须写死同值（`js/esp_shm_block.js` 顶部）：

```js
var MAGIC=0x45535031,VER=5;
var HDR_SZ=23152,ENT0=112,ENT_SZ=360,OFF_COUNT=104;
```

---

## 4. `flags` 位定义

| bit | 掩码 | 名称 | 含义 |
|---|---|---|---|
| 0 | `1` | `VISIBLE` | 在相机视锥内 / 未被遮挡 |
| 1 | `2` | `TEAM` | 与本机同队 |
| 2 | `4` | `DEAD` | 已死亡 |
| 3 | `8` | `SCREEN` | 屏幕坐标有效 |
| 4 | `16` | `LEAD` | 提前量三件套有效 |

---

## 5. 骨骼索引（`BoneIdx`）

| idx | 名称 | idx | 名称 | idx | 名称 |
|---|---|---|---|---|---|
| 0 | Head | 5 | LShoulder | 10 | LHip |
| 1 | Neck | 6 | RShoulder | 11 | RHip |
| 2 | Chest | 7 | LElbow | 12 | LKnee |
| 3 | Spine | 8 | RElbow | 13 | RKnee |
| 4 | Pelvis | 9 | — | 14 | LFoot / RFoot（末位复用） |

`Count = 15`，`bones[15][3]` = 180 B。

---

## 6. 无锁写读时序

```
writer（JS，每帧一次全量重写文件）:
    seq = 2k        ── 上一轮已收尾
    seq → 2k+1      （奇数：写进行中）
    写 view / count / ents
    seq → 2k+2      （偶数：本轮完成）

reader（C++，每帧一次 read 快照）:
    read() 全量到内存缓冲
    seq 为奇数 → 丢弃本轮
    取 count / ents[0..count)
    再核对 seq（缓冲是原子快照，理论上必然一致）
```

因为 `writeDataToFile` 是整文件覆盖写，读端读到的最坏情况是"上一轮完整文件"或"被截断的文件"，两种都由 `ESP_SHM_HDR_MIN` + `magic` + `version` + `offset + size > filesize` 拦掉，**不会画出半个方框**。

读端伪代码（与 `sensor.cpp` 一致）：

```cpp
const size_t fsz = (size_t)st.st_size;
if (fsz < (size_t)ESP_SHM_HDR_MIN) { misses_++; return false; }
read(fd, buf_.data(), fsz);
const ShmHeader* h = (const ShmHeader*)buf_.data();
if (h->magic != ESP_SHM_MAGIC) { badVersion_++; return false; }

int entSz;
if      (h->version == ESP_SHM_VERSION)          entSz = ESP_SHM_ENT_SZ_V5;  // 360
else if (h->version == 4u || h->version == 3u)   entSz = ESP_SHM_ENT_SZ_V4;  // 348
else { badVersion_++; return false; }

size_t n = h->count; if (n > ESP_SHM_MAX_ENTS) n = ESP_SHM_MAX_ENTS;
for (size_t i = 0; i < n; i++) {
    const size_t eoff = offsetof(ShmHeader, ents) + i * (size_t)entSz;
    if (eoff + (size_t)entSz > fsz) break;      // 越界即停，绝不读半条
    ShmEntity s; memcpy(&s, buf_.data() + eoff, entSz);
    ...
}
```

---

## 7. 版本兼容矩阵

| 文件 version | 写端 | 读端行为 |
|---|---|---|
| 5 | 当前 | 全字段可用（含 `camp/aiLevel/liveState`） |
| 4 / 3 | 旧 | stride=348；`camp/aiLevel/liveState` 一律按 `-1`（未知），过滤逻辑放行；`sLead/leadMeters/dropMeters/flightTime` 仍在 v4 尾部，可用 |
| 其他 | — | `badVersion_++` 丢弃本轮 |

## 8. 升级 ABI 的正确姿势

1. **只允许尾部追加**，且追加块自身要补齐到 8 字节边界；
2. 改完立刻更新 `shm_abi.h` 里的 `static_assert` 期望值；
3. 同步改 `js/esp_shm_block.js` 的 `HDR_SZ / ENT_SZ / O_*`，以及 `scripts/v6_patch.py` 里的校验字符串；
4. `ESP_SHM_VERSION` +1，读端保留旧版本分支；
5. 用 §9 的自检脚本验一遍 `sizeof` 与偏移，再上机。

## 9. 一致性自检（不编译也能验）

```python
import ctypes
BoneN = 15
class E(ctypes.LittleEndianStructure):
    _fields_=[("id",ctypes.c_uint64),
              ("head",ctypes.c_float*3),("chest",ctypes.c_float*3),("pelvis",ctypes.c_float*3),
              ("feet",ctypes.c_float*3),("velocity",ctypes.c_float*3),
              ("hp",ctypes.c_float),("maxHp",ctypes.c_float),("distance",ctypes.c_float),
              ("flags",ctypes.c_uint32),("name",ctypes.c_char*32),
              ("hasBones",ctypes.c_uint8),("screenSpace",ctypes.c_uint8),("pad",ctypes.c_uint8*2),
              ("bones",(ctypes.c_float*3)*BoneN),
              ("sHead",ctypes.c_float*2),("sChest",ctypes.c_float*2),("sFeet",ctypes.c_float*2),
              ("sLead",ctypes.c_float*2),
              ("leadMeters",ctypes.c_float),("dropMeters",ctypes.c_float),("flightTime",ctypes.c_float),
              ("camp",ctypes.c_int32),("aiLevel",ctypes.c_int32),("liveState",ctypes.c_int32)]
class V(ctypes.LittleEndianStructure):
    _fields_=[("viewProj",ctypes.c_float*16),("camPos",ctypes.c_float*3),("fovY",ctypes.c_float),
              ("viewW",ctypes.c_int32),("viewH",ctypes.c_int32)]
class H(ctypes.LittleEndianStructure):
    _fields_=[("magic",ctypes.c_uint32),("version",ctypes.c_uint32),("seq",ctypes.c_uint64),
              ("view",V),("count",ctypes.c_int32),("pad",ctypes.c_int32),("ents",E*64)]
assert ctypes.sizeof(V)==88 and ctypes.sizeof(E)==360 and H.ents.offset==112
assert ctypes.sizeof(H)==23152 and E.camp.offset==344 and E.liveState.offset==352
print("ABI OK")
```

实测输出：`ShmView 88 / ShmEntity 360 / V4 348 / ShmHeader 23152 / ents@112 / HDR_MIN 460`。

## 10. 现场抓包验证

```bash
# 文件大小应为 23152（v5 满 64 实体）或 22480（v4）
adb shell su 0 stat -c '%s %y' /data/local/tmp/esp_data.bin

# 前 16 字节：magic(31 50 53 45) version(05 00 00 00) seq(8B)
adb shell su 0 od -An -tx1 -N16 /data/local/tmp/esp_data.bin

# 拉回电脑逐实体解析（读端 stride 与写端一致才对齐）
adb pull /data/local/tmp/esp_data.bin
```
*（内容由AI生成，仅供参考）*
