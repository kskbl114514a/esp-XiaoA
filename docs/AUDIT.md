# 代码审查报告（杠精视角）

审查基线：`8aae5fc` 之后的全部本地改动一起过一遍。
审查口径：只认「能不能跑、会不会崩、行为是否与注释/文档一致」，不看风格。
结论：**改前有 7 处必崩/必错级缺陷，20+ 处会咬人的隐患**，已全部修完并双 ABI 编译通过。

---

## 0. 编译与产物验证

| 项 | 结果 |
|---|---|
| `ndk-build APP_ABI=arm64-v8a` | PASS → `libs/arm64-v8a/libesp.so` |
| `ndk-build APP_ABI=x86_64` | PASS → `libs/x86_64/libesp.so` |
| `scripts/v7_patch.py` | 实跑 PASS，25 项字符串校验全绿，括号配平 `{ ( [` 全 0 |
| `jni/src` 不安全 C 函数（strcpy/sprintf/gets/strcat/alloca） | 0 命中 |

---

## 1. 必崩 / 必错级（P0）

### 1.1 `js/esp_shm_block.js` —— 写文件参数顺序反了，`esp_data.bin` 从来没生成过
- 现象：每帧 logcat 刷 `jsb_conversions_spec.cpp:312 sevalue_to_native` + `Convert parameter to Data failed!` + `Failed to invoke js_cc_FileUtils_writeDataToFile`，`files/esp_data.bin` 不存在。
- 根因：`writeDataToFile(path, data)` 被写成 `writeDataToFile(path, u8)` 反序；且 C++ 层失败**不抛 JS 异常**，旧代码 `return !0` 直接谎报成功，所以上层永远以为写进去了。
- 修法：`fu.writeDataToFile(buf, path)` 优先、失败回退 `u8`；写完调 `getFileSize` 校验 `ioSz === HDR_SZ`，不等就 `return false`；日志补 `sz=`。

### 1.2 同上文件 —— JS 侧 ABI 常量落后一版，第 2 个实体起整体错位 4 字节
- 根因：设备上的 `index_v6.js` 用 `HDR_SZ=22896 / ENT_SZ=356`，C++ `shm_abi.h` 是 `23152 / 360`。
- 修法：`HDR_SZ=23152, ENT0=112, ENT_SZ=360, OFF_COUNT=104`；`O_CAMP=344 / O_AI=348 / O_LS=352`；`v7_patch.py` 的校验串同步从 `entsz356` 改 `entsz360`。

### 1.3 `evtouch.cpp` —— 自瞄抬手会清掉玩家的 `BTN_TOUCH`，真实手指全被吞
- 根因：`AimTouchUp()` 里 `anyPhys` 只看 `g_slot[i].vDown`，而"没抓到物理设备"时该标志恒 false → 判定"没有手指"→ `BTN_TOUCH=0`。玩家手指还按着，内核却收到 ACTION_UP，之后的 MOVE 全部作废，表现为"瞄一下人就不能动了"。
- 修法：判据改 `(down || vDown)`，任一为真就保持 `BTN_TOUCH=1`。

### 1.4 `touch.cpp` —— 单 bool 消费态在多指下串味
- 根因：`g_consuming` 是个全局 bool。手指 A 在菜单里按下置位，手指 B 从菜单外滑进来也会被当成"在操作菜单"，凭空产生一条没有 DOWN 的 MOVE，把游戏拖动吃掉。
- 修法：改 `std::atomic<int> g_consumeId`，`Down/Move/Up/Cancel` 全部按 `pointerId` 比对；只有当初按下的那根手指能继续喂 ImGui。

### 1.5 `render.cpp` —— 事件队列满时把 UP/CANCEL 丢掉，ImGui 永久卡在按下态
- 根因：`InjectRawTouch` 队列 128 满了就无条件 drop。丢的要是抬手事件，ImGui 认为手指永远按着：滑块粘手、按钮常亮、后续点击全部错位。
- 修法：满时普通事件直接累加丢弃计数；`UP/CANCEL` 则先驱逐一条 `action==2` 的 MOVE 腾位再 push，保证抬手必达。

### 1.6 `menu.cpp` —— 自瞄 20 个参数根本不落盘
- 根因：`Save()` 只写透视/弹道那批键，`aim*` 一个没写；`Load()` 也没有对应 `KBOOL/KF`。菜单里调好的跟手增益、锁定半径、落点位置，重启即灰飞烟灭。
- 修法：`Save` 补 22 个 `aim*` 键，`Load` 补 8 个 `KBOOL` / 1 个 `KINT` / 12 个 `KF`。

### 1.7 `entry.cpp` + `Android.mk` —— 触摸自瞄压根没接线
- 根因：`DrawOneFrame` 只调 `Esp/Track/Menu`，**没有任何 `TouchAim` 调用**；`Android.mk` 的 `LOCAL_SRC_FILES` 也不含 `touchaim.cpp`。等于这个模块此前是纯摆件。
- 修法：`Android.mk` 加 `src/touchaim.cpp` + `src/aimbot.cpp`；`entry.cpp` 补 `#include "touchaim.h"`、在 `Track::Draw` 与 `Menu::Draw` 之间插 `TouchAim::Update(view, ents, dt)`、`OnUnload` 补 `TouchAim::Reset()`。

---

## 2. 会咬人的隐患（P1）

### 2.1 `sensor.cpp`
| 缺陷 | 修法 |
|---|---|
| 包名读空时路径退化成 `/esp_data.bin` | 空则回落 `/data/local/tmp` |
| `Init` 无条件执行，重复构造 | `inited_` 幂等 |
| 读到撕裂快照照单全收 | `ReadSeqFresh()` 重新 open/read 比对 seq，不一致丢弃并 `tear_++` |
| `viewW/H`、`fovY` 不校验，坏值直接进投影 | `fovY` 夹 (1,170)，`viewW/H` 出 `[16,8192]` 置 0 并 `badView_++` |
| `hasProj` 从未被设置，等于恒 false | 由 16 个投影矩阵 float 平方和判定 |
| `name` 无终止符，越界读 | `e.name[sizeof-1] = 0` |

### 2.2 `bullet_track.cpp` —— 锁头逻辑与需求相反
- 根因：`Vec3 tgt = (g_cfg.btBone==1) ? e.chest : e.head;` 紧接 `if (tgt 全 0) tgt = e.chest;`。**头点无效时直接退化成胸**，用户明确要求"必须锁头不锁胸"，这里反着来。
- 修法：新增 `Zero3()/ScrValid01()/g_fallbackPts`，头点无效时用 `feet + 1.72f`（人体头部高度）重建，绝不退胸口；`screenSpace` 分支失败再用 `v.hasProj && W2S` 兜底；另补 `isKnocked/btIgnoreTeam` 过滤、提前量分支 `h.dist = Dist3(v.camPos, tgt)` + `h.useMatrix=false`、`Draw` 门禁改 `g_hit.useMatrix && v.hasProj && inView`、`Reset` 清计数。

### 2.3 `esp.cpp`
- `screenSpace` 分支不校验归一化值就乘屏宽 → 画出屏幕外的鬼影框：加 `NormOk()`。
- 最小框 4px 硬编码在小屏糊成一片：`MinBoxPx()` 改 `3.f * UiScale`。
- 骨盆直接取胸、姿态跳变：改 `feet → chest` 的 0.45 插值。
- 无 `hasProj` 仍进绘制：直接 return。
- 距离开方在循环里白算：过滤提前。
- 框宽无上限、名字 `x` 不夹紧：`ClampF(boxRatio, 0.20f, 1.20f)` + 名字 x 夹紧。

### 2.4 `menu.cpp` 持久化
- `CfgPath()` 硬编码包名 → 改 `SelfPkg()` 读 `/proc/self/cmdline`，主路径 `/sdcard/Android/data/<pkg>/files/esp_cfg.ini`，备用 `/data/local/tmp/esp_cfg.ini`。
- `Save()` 非原子，断电留半截文件 → `.tmp` → `fflush` + `fsync` + `rename`。
- `Load()` 不处理行尾 `\r`，Windows 手改 ini 会整片失配 → 统一剥 `\r\n`；未知键计数。
- ini 可手改，越界值直接送进渲染/触摸（`btBone=7`、`uiScale=0`、`aimGain=50` 能把手指甩出屏幕）→ 加载后统一夹紧 20+ 项。
- UI 缺"跳过队友""框宽比例"→ 补；`btBone` Combo 前先夹紧。
- `aimEnabled` 被硬写 `false` 覆盖 ini → 删掉强关，改夹紧。

### 2.5 `java_bridge.cpp` —— 每个 Attach 过的线程永生不解附
- 根因：只有 `AttachCurrentThread`，没有 `DetachCurrentThread`；`FindClass` 拿到的是 LocalRef 却当全局用；`NewObject` 失败分支泄漏 `pp/pc`；三个失败分支漏 `DeleteLocalRef`。
- 修法：`FindClass` 结果 `NewGlobalRef` 提升为全局再释放局部；失败分支补齐释放；新增 `pthread_key_t g_tlKey` + `pthread_once` + 线程析构 `TlDetach()`，线程退出自动 `DetachCurrentThread`。另修 `Dispatch` 改 `static bool`、新增 `g_uiDrag` 门禁（拖动中不投自瞄，否则两个通道抢同一根手指）。

### 2.6 `touchaim.cpp`
- `AimPoint` 取头点为空时静默锁胸 → 新增 `Valid3()/HeadFallback()`，兜 `feet+1.72f`。
- `AimTouchMove` 失败分支不抬手 → 显式 `EvdevTouch::AimTouchUp()`，避免虚拟手指永远按住屏幕。
- `Update()` 开头 `g.lastDelta=0` 每帧清零 → 删。
- 无目标时不复位 → 清 `lastDelta/tgtX/tgtY`。
- 早退分支没清 `g_picked/targetId` → 合并为 `if(!v.valid||!v.hasProj)` 一起清。
- 提前量衰减用屏幕像素而非米 → `WithPredict(const Entity&, Vec3, float distM)`，`ClampF(distM/bulletSpeed, 0, 3)` + `0.5*btGravity*t*t`。

### 2.7 其它
- `aimbot.cpp`：`CurrentTarget()` 返回局部、`HasTarget()` 不看 `g_targetId`、`PxPerDeg()` 用全屏宽算度数、粘滞分支 `break` 位置错致漏过滤、`dt` 不夹。→ 全修，并改用屏幕半高。
- `vecmath.h`：`W2S` 不校验 `screenW/H<=0`、除零 `cw<1e-4`、`ndc` 非有限值、返回值不校验；`ScreenToAngle` 的 `d` 可能 NaN → 全加 `isfinite` 早退。
- `hook.cpp`：`FindModule` 回调里留了句 `d->second->first = d->second->first;` 自赋值废语句 → 删。
- `touchaim.h` 缺 `<vector>`；`config.h` 第 48 行注释写"默认开"，实际 `aimEnabled=false` → 改注释。

---

## 3. 仓库与脚本层

| 问题 | 处置 |
|---|---|
| `scripts/v7_patch.py` 的 `BLOCK` 指向 `scripts/esp_shm_block.js`，**仓库里根本没这个文件**，直接跑必 `FileNotFoundError` | 回退到 `../js/esp_shm_block.js`（唯一事实源），并在入口加基线/块存在性断言；docstring 里 "v6 / index_v6.js" 改成 v7 |
| `scripts/v6_patch.py` 与 v7 近乎逐行重复，且断言还是旧的 `356` | 删除 |
| `scripts/esp_v5.sh`、`scripts/esp_a64.sh` 已并入 `scripts/esp.sh` 却仍在仓库里 | 删除 |
| `scripts/deploy_arm64.ps1` 仍在调用 `esp_a64.sh`、提示语指向 `esp_v5.sh` | 全部改 `esp.sh` |
| `docs/TROUBLESHOOT.md` 6 处仍写 `esp_v5.sh` / `esp_a64.sh` | 改 `esp.sh`（`libesp_a64.so` 是产物名，不动） |
| `aimbot.cpp` 与 `touchaim.cpp` 两套自瞄并存，前者不在编译列表里 = 死代码 | 纳入 `Android.mk`（保证它能编译、不留腐烂代码），并在 `aimbot.h` 顶部注明是「uinput 被 ROM 屏蔽时的备用 Java MotionEvent 通道」 |

---

## 4. 已知未处理（留档，不当成已修）

1. `esp.sh verify` 曾在真机上出现一次 `Segmentation fault`，未复现、未定位。
2. JS 写入端未实现 `shm_abi.h` 注释里描述的「写奇数 seq → 写数据 → 写偶数 seq」双阶段协议，目前是直写 + 读端 seq 重读比对兜底。撕裂窗口仍在（毫秒级）。
3. `SensorShm path=` 同一路径会打印两次，疑似重复构造，未查。
4. `hideKnocked` 依赖 `hp<=0`，倒地期若 JS 侧血量非 0 则该开关失效，需要在真机对局里确认 JS 的实际取值。
5. `inj_a64` 仅静态编译通过，未真机实测；`FindRemoteSym` 要求完整路径匹配。
