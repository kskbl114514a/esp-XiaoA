---
AIGC:
    Label: "1"
    ContentProducer: 001191440300708461136T1XGW3
    ProduceID: c7bba7cd687c3be43ccd054e5d679f26_1a590b34e14d11f1913b525400393706
    ReservedCode1: I53z9pmt99qjxIQ4Kh1NXhKlIlIndyXBelGTtkPEPdUXjz6neL0qyu4rvR0J38fg/HmfXOfWf0WhQz5NtWsJEpqhW+u5j7JpIX/arA1q4BlUEwNzh3Le8cEAfqQLpNxSowzyXVT4fXWvX1rxZYEldphcD7p9QFT16wZgKXpXTLyVi/NZpzaBJwo+AEQ=
    ContentPropagator: 001191440300708461136T1XGW3
    PropagateID: c7bba7cd687c3be43ccd054e5d679f26_1a590b34e14d11f1913b525400393706
    ReservedCode2: I53z9pmt99qjxIQ4Kh1NXhKlIlIndyXBelGTtkPEPdUXjz6neL0qyu4rvR0J38fg/HmfXOfWf0WhQz5NtWsJEpqhW+u5j7JpIX/arA1q4BlUEwNzh3Le8cEAfqQLpNxSowzyXVT4fXWvX1rxZYEldphcD7p9QFT16wZgKXpXTLyVi/NZpzaBJwo+AEQ=
---

# esp-XiaoA

> XiaoA 版 ESP 框架 —— Cocos Creator 3.x 手游的**原生 ImGui 悬浮菜单 + 实体透视线框**。

Cocos Creator 3.x 手游的 **原生 ImGui 悬浮菜单 + 实体透视线框** 框架。

核心思路：**数据采集交给 JS，绘制交给 Native**。游戏侧的 JS 运行时把场景实体的世界坐标、屏幕坐标、血量、阵营、AI 等级等原始数据写入一块共享内存文件；Native 侧用 inline hook 拿到 `eglSwapBuffers`，在 ImGui 里读这块内存画方框、天线、人数、提前量点。

两端只靠一个固定布局的二进制结构体通信，**互不依赖对方的运行时**——JS 侧崩了 native 面板照常显示（只是没数据），native 侧换了 ImGui 版本也不影响 JS。

```
┌──────────────── 游戏进程 (Cocos Creator 3.8.x) ────────────────┐
│                                                                │
│  JS 运行时 (SystemJS bundle)           Native (libesp.so)       │
│  ┌────────────────────────┐           ┌──────────────────────┐ │
│  │ esp_shm_block.js       │           │ entry.cpp  init      │ │
│  │  · 每帧扫 SceneGraph   │           │ hook.cpp  eglSwap-   │ │
│  │  · worldToScreen       │           │           Buffers    │ │
│  │  · 算提前量 / 下坠     │           │ render.cpp ImGui ES3  │ │
│  │  · fwrite + seq 双写   │           │ menu.cpp  悬浮面板    │ │
│  └───────────┬────────────┘           │ esp.cpp   Draw 循环   │ │
│              │                        │ sensor.cpp 读 + 过滤  │ │
│              ▼                        └──────────▲───────────┘ │
│   esp_data.bin (mmap / 文件)  ───────────────────┘              │
│   ShmHeader{magic,version,seq,view,count,ents[64]}              │
└────────────────────────────────────────────────────────────────┘
```

## 特性

- **纯 native 绘制**：ImGui + OpenGL ES 3.x，`eglSwapBuffers` 内联 hook，不落地任何 Java 层 View，不进 Zygote。
- **数据面与绘制面解耦**：ABI 版本化（当前 v5），读端按 `version` 字段自动选择实体步长，能直接读 v3/v4 写出来的旧文件。
- **无锁双写校验**：写入端 `seq+1(奇)` → 写数据 → `seq+2(偶)`，读端只在偶数且前后一致时采信，不需要 mutex/信号量。
- **屏幕坐标预计算**：`worldToScreen` 在 JS 侧完成，写归一化 `0..1` 屏幕坐标，读端乘视口尺寸即得像素，与设备分辨率/画布缩放彻底解耦。
- **子弹提前量在 JS 侧解算**：飞行时间、下坠、落点屏幕坐标都算好再写进来，C++ 只管画圈和十字。
- **两套 ABI 预编译**：`x86_64`（模拟器）与 `arm64-v8a`（真机），脚本自动校验 `e_machine` 防推错架构。
- **一体化运维脚本**：单文件 `esp_v5.sh`，`all / inject / verify / rollback / status` 五个子命令，幂等可重入。

## 目录结构

```
.
├── jni/                        NDK 工程（ndk-build）
│   ├── Android.mk
│   ├── Application.mk          APP_ABI = arm64-v8a x86_64, android-24, c++_static
│   ├── imgui/                  ImGui 1.89+ docking 分支（含 OpenGL3 后端）
│   ├── third_party/            And64InlineHook（arm64 inline hook）
│   └── src/
│       ├── shm_abi.h           ★ 数据通道 ABI 定义（两端唯一契约）
│       ├── entry.cpp           库入口 / 工作线程 / 字体加载
│       ├── hook.cpp            eglSwapBuffers inline hook
│       ├── render.cpp          GL 状态保存 + ImGui 渲染循环
│       ├── menu.cpp            悬浮面板 UI
│       ├── sensor.cpp          读 esp_data.bin + 数据过滤 + 投影
│       ├── sensor_native.cpp   原生侧兜底采集（无 JS 时的 SceneGraph 扫描）
│       ├── esp.cpp             方框 / 天线 / 血量 / 距离 / 提前量绘制
│       ├── bullet_track.cpp    弹道预解算与绘制
│       ├── aimbot.cpp          自瞄数学（角度插值 / 平滑）
│       ├── touchaim.cpp        触摸式自瞄
│       ├── touch.cpp           触摸事件注入
│       ├── evtouch.cpp         /dev/input 事件注入通道
│       ├── java_bridge.cpp     JNI 辅助（JavaVM / Activity 获取）
│       ├── config.h            ★ 运行期配置与默认值
│       ├── vecmath.h           向量 / 矩阵 / 投影
│       └── log.h               logcat 封装
├── injector/
│   ├── injector_x64.c          ptrace 注入器（x86_64，模拟器 / houdini 环境）
│   ├── injector_arm64.c        ptrace 注入器（aarch64，真机；AAPCS64 + user_pt_regs）
│   └── injector.cpp            早期原型（保留参考）
├── js/
│   └── esp_shm_block.js        ★ 注入到游戏 bundle 的采集块（约 14 KB）
├── scripts/
│   ├── esp_v5.sh               设备端一体化脚本（模拟器 x86_64）
│   ├── esp_a64.sh              设备端一体化脚本（真机 arm64 + KSU/Magisk）
│   ├── deploy_arm64.ps1        Windows 侧一键部署（校验架构/md5 → push → chmod）
│   └── v6_patch.py             幂等打补丁器：把 esp_shm_block.js 插进 index.js
├── prebuilt/
│   ├── libesp_x86_64.so        1,397,400 B  x86_64（模拟器）
│   ├── libesp_arm64-v8a.so     1,292,680 B  aarch64（真机）
│   ├── injector_x64            12,816 B     x86_64 注入器（模拟器，必须用这个）
│   └── arm64/inj_a64           11,080 B     aarch64 注入器（真机）
└── docs/
    ├── ABI.md                  布局全表 + 读写时序
    └── TROUBLESHOOT.md         实战排障记录
```

## 快速开始

### 0. 前置

| 项 | 要求 |
|---|---|
| NDK | r25.2.9519653（其他 r25/r26 亦可） |
| 设备 | Android 7.0+ / `arm64-v8a` 或 `x86_64` / **已 root**（`su 0` 可用） |
| 目标 | Cocos Creator 3.x，JS bundle 可读可写（明文 `index.js`，非加密包） |

### 1. 编译

```bash
cd jni
ndk-build.cmd NDK_PROJECT_PATH=. \
  APP_BUILD_SCRIPT=Android.mk \
  NDK_APPLICATION_MK=Application.mk -j4
# 产物：libs/x86_64/libesp.so  libs/arm64-v8a/libesp.so
```

`Android.mk` 里已在 `LOCAL_LDLIBS` 声明 `-llog -landroid -lEGL -lGLESv3 -lGLESv2 -ldl -lm`；**不要去掉 `-lGLESv3`，否则 `glDrawElements` 系列符号在 android-24 上会解析失败**。

### 2. 推文件

设备上统一放一个目录，脚本内 `BASE` 常量指向它（默认 `/data/local/tmp/imgui`）：

```
/data/local/tmp/imgui/
├── esp_v5.sh                 chmod 755
├── inj_a                     chmod 755   ← prebuilt/injector_x64
├── libesp_only.so            chmod 644   ← prebuilt/libesp_x86_64.so（按 ABI 选）
├── libesp_only_arm64.so      chmod 644   ← prebuilt/libesp_arm64-v8a.so
├── index_v6.js               chmod 644   ← 打完补丁的新 index.js
└── index_v3.js               chmod 644   ← 干净基线，rollback 用
```

```bash
adb shell mkdir -p /data/local/tmp/imgui
adb push esp_v5.sh        /data/local/tmp/imgui/esp_v5.sh
adb push injector_x64     /data/local/tmp/imgui/inj_a
adb push libesp_x86_64.so /data/local/tmp/imgui/libesp_only.so
adb push index_v6.js      /data/local/tmp/imgui/index_v6.js
adb push index_v3.js      /data/local/tmp/imgui/index_v3.js
adb shell chmod 755 /data/local/tmp/imgui/esp_v5.sh /data/local/tmp/imgui/inj_a
```

### 3. 打 JS 补丁

`scripts/v6_patch.py` 是幂等的：对同一份 `index.js` 反复跑不会重复插入。

```bash
python scripts/v6_patch.py \
  --in  index_original.js \
  --out index_v6.js \
  --block js/esp_shm_block.js
```

补丁做的事情：

1. 在 bundle 尾部插入 `esp_shm_block.js` 全文；
2. 包一层 `try/catch`，采集块内部任何异常都不会打断游戏主循环；
3. 注入 `__ESP_CFG__ / __SPD_CFG__ / __JUMP_CFG__ / __RATE_CFG__ / __AIM_CFG__ / __LS_CFG__` 六个全局配置对象，运行时可在 native 面板改。

### 4. 跑

```bash
adb shell su 0 sh /data/local/tmp/imgui/esp_v5.sh             # 换 JS + 冷启动 + 注入 + 体检
adb shell su 0 sh /data/local/tmp/imgui/esp_v5.sh all nojs     # 不换 JS，只注入
adb shell su 0 sh /data/local/tmp/imgui/esp_v5.sh inject       # 只注入当前进程
adb shell su 0 sh /data/local/tmp/imgui/esp_v5.sh verify       # 只读体检
adb shell su 0 sh /data/local/tmp/imgui/esp_v5.sh rollback     # 恢复基线 index.js
adb shell su 0 sh /data/local/tmp/imgui/esp_v5.sh status       # 三件套文件状态 + ABI 自检
```

脚本行为要点：

- **注入前查 `/proc/<pid>/maps`**，已映射则跳过，天然幂等，不会重复注入污染进程；
- **`e_machine` 自检**：读 so 的第 18–19 字节，`3e00` = x86_64，`b700` = aarch64，与 `getprop ro.product.cpu.abi` 比对，不匹配直接 WARN；
- 换 JS 时先备份到 `index.js.bak_pre_v6`，写回后强制 `chmod 600` + `restorecon` + `chcon u:object_r:app_data_file:s0`。

## 真机 arm64 部署（root + KernelSU）

模拟器（x86_64 + houdini）那一套**不能**直接搬到真机。真机是纯 arm64 环境，缺两样东西：**arm64 版注入器**，以及**能被目标进程合法 dlopen 的 so 路径**。本仓库两条都已备好。

### 与模拟器的差异对照

| 项目 | 模拟器 / houdini | 真机 arm64 |
|---|---|---|
| 注入器 | `prebuilt/injector_x64`（12,816 B, x86_64） | `prebuilt/arm64/inj_a64`（11,080 B, aarch64） |
| 被注入模块 | `prebuilt/libesp_x86_64.so` | `prebuilt/libesp_arm64-v8a.so` |
| 寄存器读法 | x86_64 `struct user_regs_struct` + SysV 传参 | arm64 `struct user_pt_regs`(272B) + AAPCS64(x0..x7) |
| 返回哨兵 | 栈上写 0，`ret` 跳 0 | `x30(LR)=0`，`ret`/`br x30` 跳 0 |
| 栈对齐 | `rsp%16==8`（有返回地址） | `sp` 必须 16 对齐（无栈上返回地址） |
| so 放置位置 | `/data/local/tmp/imgui/` 可直接 dlopen | **必须**进 App native lib 目录（见下） |
| 提权 | `su 0 sh ...` | `su -c "sh ..."`（KSU/Magisk 均可） |

### 1. 编 arm64 注入器

```bash
cd injector
$NDK/ndk-build.cmd NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk \
                  NDK_APPLICATION_MK=Application.mk NDK_OUT=obj NDK_LIBS_OUT=libs -j4
# 产物: libs/arm64-v8a/inj_a64  (aarch64, PIE, md5 c109c3460b2fa62750035d7994b92d0a)
#       libs/x86_64/inj_x64      (模拟器那份，同一工程一并出)
```

`injector/Android.mk` 按 `TARGET_ARCH_ABI` 条件构建两个模块，互不干扰：`injector_arm64.c` 里有 `#error` 守卫，误编到 x86_64 会直接报错而不是产出垃圾。

### 2. 推文件

```bash
D=/data/local/tmp/imgui
adb push prebuilt/arm64/inj_a64        $D/inj_a64
adb push prebuilt/libesp_arm64-v8a.so  $D/libesp_arm64.so
adb push scripts/esp_a64.sh            $D/esp_a64.sh
adb push js/esp_shm_block.js           $D/index_v6.js
adb shell su -c "chmod 755 $D/inj_a64; ls -l $D"
```

### 3. 关键坑：so 不能放在 /data/local/tmp

真机上 `dlopen("/data/local/tmp/imgui/libesp.so")` 会被两重机制挡掉：

1. **linker namespace**：`library "..." is not accessible for the namespace "classloader-namespace"` —— App 进程的 namespace 只允许 `/data/app/<pkg>*/lib/arm64`、`/system/lib64`、`/apex/...` 等路径；
2. **SELinux**：`/data/local/tmp` 是 `shell_data_file`，app 域无读权限（`avc: denied { read }`）。

所以脚本会把 so 铺到 App 的 native lib 目录再 dlopen：

```bash
LIBDIR=$(grep -m1 '/lib/arm64/' /proc/$PID/maps | awk '{print $6}' | xargs dirname)
cp -f $D/libesp_arm64.so $LIBDIR/libesp_a64.so
chmod 644 $LIBDIR/libesp_a64.so && restorecon $LIBDIR/libesp_a64.so
# 注入目标就是 $LIBDIR/libesp_a64.so
```

lib 目录名带随机 hash（`/data/app/~~xxxx==/<pkg>-yyyy==/lib/arm64`），**不能写死**，脚本用运行中进程的 maps 反查。

### 4. 跑

```bash
# 一步到位（换 JS + 冷启动；auto 会等 45s 供你进对局后自动注入）
adb shell su -c "sh /data/local/tmp/imgui/esp_a64.sh all auto"

# 或者手动控时序：起游戏 → 进对局/训练场 → 注入
adb shell su -c "sh /data/local/tmp/imgui/esp_a64.sh all nojs"
adb shell su -c "sh /data/local/tmp/imgui/esp_a64.sh inject"   # 进对局后再执行
adb shell su -c "sh /data/local/tmp/imgui/esp_a64.sh verify"
```

时序铁律：**大厅里 `Scene`/`Camera` 指针为 0，vtable 扫描命中 0 次**，必须进对局或训练场再注入；模块内每 30 帧复检 `pickCam()`，相机换指针会自动重建层，所以即便早注入了，切进对局也会自己补上。

### 5. 验收

```bash
grep -c libesp_a64.so /proc/$(pidof com.fmgame.pubg.aligames)/maps   # 必须 = 3
logcat -d -s esp:V | grep -E "esp.so|hook\(|GL_VERSION|ImGui|surface|\[shm\]"
```

映射段数为 0 时，先看 `logcat -d | grep -E "not accessible for the namespace|avc: *denied"`：
- 命中 namespace → so 没进 lib 目录（stage 失败 / 用了 `/data/local/tmp` 路径）
- 命中 avc → `restorecon` 没做或 SELinux 策略过严，必要时临时 `setenforce 0` 验证

## 数据通道 ABI (v5)

完整定义见 [`jni/src/shm_abi.h`](jni/src/shm_abi.h)，布局全表与时序见 [`docs/ABI.md`](docs/ABI.md)。

```c
#define ESP_SHM_MAGIC   0x45535031u   // "ESP1"
#define ESP_SHM_VERSION 5
#define ESP_SHM_MAX_ENTS 64
```

| 结构 | 尺寸 | 关键偏移 |
|---|---|---|
| `ShmView` | 88 B | `viewProj[16]@0` `camPos@64` `fovY@76` `viewW@80` `viewH@84` |
| `ShmEntity` | **360 B**（v4/v3 为 348 B） | `id@0` `head@8` `chest@20` `pelvis@32` `feet@44` `velocity@56` `hp@68` `maxHp@72` `distance@76` `flags@80` `name@84` `hasBones@116` `screenSpace@117` `pad[2]@118` `bones[15][3]@120` `sHead@300` `sChest@308` `sFeet@316` `sLead@324` `leadMeters@332` `dropMeters@336` `flightTime@340` **`camp@344` `aiLevel@348` `liveState@352`** |
| `ShmHeader` | **23,152 B** | `magic@0` `version@4` `seq@8` `view@16` `count@104` `pad@108` `ents@112` |

> 注意 `id`（`uint64_t`）与 `bones` 之后的 4 字节对齐填充：结构体整体按 8 字节对齐，`360 - 356 = 4` 就是尾部 padding。所以**别用字段长度相加去推 sizeof**，以 `sizeof()` / `docs/ABI.md` 的表为准。

v5 相对 v4 只在**尾部**追加 12 字节（三个 `int32`），前面偏移完全不变，所以：

- 读端按 `h->version` 选步长：`v5 → 360`，`v4/v3 → 348`（`ESP_SHM_ENT_SZ_V4 = sizeof(ShmEntity) - 12`）；
- 最小可读头长度 `ESP_SHM_HDR_MIN = offsetof(ShmHeader, ents) + 348 = 460` 字节，文件小于它直接丢弃本轮；
- 读端用 `offsetof + memcpy` 取值，并在 `offset + size > filesize` 时 `break`，不会越界读；
- 写端还是老版本时，读端把 `camp/aiLevel/liveState` 一律视为 `-1`（未知），过滤逻辑自动放行。

`flags` 位定义：

| bit | 名称 | 含义 |
|---|---|---|
| 0 | `VISIBLE` | 该实体在相机视锥内 |
| 1 | `TEAM` | 与本地玩家同队 |
| 2 | `DEAD` | 已死亡 |
| 3 | `SCREEN` | `sHead/sChest/sFeet` 有效（0..1 归一化） |
| 4 | `LEAD` | `sLead/leadMeters/dropMeters/flightTime` 有效 |

### 读写时序（无锁）

```
writer:  seq = 2k   → 写 seq = 2k+1 (奇数)  → 写 view / count / ents → 写 seq = 2k+2 (偶数)
reader:  读 seq；若为奇数则跳过本轮；读数据；再读 seq，前后相等才采信
```

读者永远在 `seq` 为偶数且前后一致时采信，写入端被抢占/半写时读者丢弃该轮，不会画出半个方框。

## 面板

收起态是左上角一个蓝色圆钮（`p(18*s, 150*s)`，`r = 30*s`，文字「透视」），点开后是完整面板：

| 分组 | 项 | 默认 |
|---|---|---|
| 透视 | 玩家方框 / 天线 / 血量条 / 距离 / 骨骼 | 方框、距离开 |
| 数据过滤 | **隐藏 AI**（`hideAI`，判据 `aiLevel > 0`） | `true` |
| | **隐藏倒地**（`hideKnocked`，判据 `liveState == 2`） | `true` |
| | **仅敌人**（`onlyEnemy`，判据 `camp != 本地 camp`） | `false` |
| 提前量 | 提前量点（品红外圈 + 十字，`colLead = {1.00, 0.35, 1.00, 1.0}`） | `true` |
| 数值 | 移速倍率 / 跳跃倍率 / 射速倍率 | 3x / 4x / 2x |
| 自瞄 | 触摸自瞄开关 / 平滑 / 部位（头/胸） | 关 |

面板配置 `Save/Load` 会连同 `hideAI / hideKnocked / onlyEnemy / showTag / drawLeadPoint / colLead` 一起持久化。

过滤逻辑在 `esp.cpp::Draw` 的循环首部三条 `continue`，**这一层才是最终裁决者**——JS 侧不做丢弃，保证面板开关随时可逆。

## 数据来源与过滤语义

JS 侧从 `CharacterCom.data` 原样透传三个字段，不做任何裁剪：

```js
// v5：全量透传，onlyReal / skipDead 都置 false，交给 native 面板决定
{ camp: d.camp, aiLevel: d.aiLevel, liveState: d.liveState }
```

| 字段 | 取值 | 语义 |
|---|---|---|
| `camp` | 2 = 与本地玩家同营 | 无 `teamId`，用阵营值相同判定队友 |
| `aiLevel` | `> 0` | AI / BOT |
| `liveState` | `0` 活 / `1` 死 / `2` 倒地 / `-1` 未知 | 倒地期 `hp` 可能已归 0 |

派生规则：`isAI = (aiLevel > 0)`、`isKnocked = (liveState == 2)`。

## 验证清单

只信三个硬指标，别信"注入成功"的文字输出：

```bash
# 1. so 真的映射进进程（唯一验收标准），应为 3 段
adb shell su 0 sh -c 'grep -c libesp_only.so /proc/$(pidof <包名>)/maps'

# 2. 日志链齐全
adb logcat -d -s esp:V | grep -E 'esp\.so|hook\(|GL_VERSION|ImGui|surface|\[shm\]|Perf'

# 3. 数据在刷新（mtime 应变化）
adb shell stat -c '%s %y' /data/local/tmp/esp_data.bin
```

正常启动日志序列：

```
esp.so 已加载
工作线程启动
hook(x64) ok
GL_VERSION=OpenGL ES 3.1 v1
中文字体加载成功
ImGui 就绪 (#version 300 es)
surface=1920x1080
[shm] path=/data/local/tmp/esp_data.bin
数据源就绪：Cocos3.x SceneGraph(auto)
```

## 坑

**1. 注入器版本决定生死。**
`prebuilt/injector_x64`（12,816 B，md5 `21da0af9165c9a04b0c8d00edfa4459e`）是唯一验证过能用的版本。另一份 17,328 B 的注入器 `RemoteCall` 少减了 8 字节，`movaps` 撞栈破坏 16 字节对齐，结果：注入器自己报成功、返假句柄、`maps` 计数 0、目标进程不崩但什么都没发生。**这种问题静态编译期完全看不出来，只能真机冒烟。**

**2. so 架构必须和设备 ABI 对齐，但别用 `ro.product.cpu.abi` 直接推断。**
雷电模拟器 `ro.product.cpu.abi = x86_64`，游戏进程的 `libc` 却可能落在 `/system/vendor/lib64/arm64/libc.so`（arm64 跑在 houdini 转译层上）——但**注入的 so 必须是 x86_64 版**。判据只有 `e_machine`：

```bash
od -An -tx1 -N2 -j18 libesp.so     # 3e00 = x86_64 / b700 = aarch64
```

**3. 同一份源码不同构建产物大小不同，别拿文件大小判断版本。**
`libesp_only.so` 在两次构建里分别是 1,407,936 B 和 1,292,680 B，都能跑。一律 `md5` + `maps` 双证。

**4. 触摸接管拿不到多点。**
`Java_com_cocos_lib_CocosTouchHandler_handleAction*` 四个符号在目标里全不存在，回落 `MotionEvent` 通道可行，但**只支持 `pointerId = 0`**，会和真实手指抢输入。面板交互和边走路边开菜单无法同时成立。

**5. Cocos 的坐标系陷阱。**
`worldToScreen` 是左下原点、y 向上，屏幕坐标是左上原点、y 向下，必须翻转；`cc.UITransform` 不在 `cc` 全局上，要 `addComponent("cc.UITransform")`。

更多见 [`docs/TROUBLESHOOT.md`](docs/TROUBLESHOOT.md)。

## 已知限制

- 只对 **明文 JS bundle** 有效（Cocos 加固 / 加密 bundle 需要另外脱，不在此项目范围）。
- `sensor_native.cpp` 的原生兜底采集在部分 Cocos 版本上拿不到 `worldRoot`（日志会打 `no main camera`），此时必须依赖 JS 侧写数据。
- 目标分辨率变化（横竖屏切换 / 分屏）后需等一帧 `surface` 重建，`pickCam()` 每 30 帧校验一次相机变化并重建层。
- 面板字体用的是内置中文子集，新增字符可能显示成方块。

## 许可

MIT，见 [LICENSE](LICENSE)。

第三方组件：Dear ImGui（MIT）、And64InlineHook（Apache-2.0）。
*（内容由AI生成，仅供参考）*
