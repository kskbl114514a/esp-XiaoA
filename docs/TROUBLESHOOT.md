---
AIGC:
    Label: "1"
    ContentProducer: 001191440300708461136T1XGW3
    ProduceID: c7bba7cd687c3be43ccd054e5d679f26_1c763faae14d11f189bc525400248c00
    ReservedCode1: izzyDwnmnCRWmFL9zgkAygT5GXrNmrudTEq8Z4DhcMIuhmLmwl+JldaCzOCEAA2gVrZHId6hyRV5ilkVfzidz0awK+i+MvEqAfoNcEWaUqVQrk+3jYbI5Mn84rLkxDb/lBGWB12JVY8z7IVeQMfVSy965YZa3m4fKN8n+S2mr+5RGy6RIn6M45Vxzqw=
    ContentPropagator: 001191440300708461136T1XGW3
    PropagateID: c7bba7cd687c3be43ccd054e5d679f26_1c763faae14d11f189bc525400248c00
    ReservedCode2: izzyDwnmnCRWmFL9zgkAygT5GXrNmrudTEq8Z4DhcMIuhmLmwl+JldaCzOCEAA2gVrZHId6hyRV5ilkVfzidz0awK+i+MvEqAfoNcEWaUqVQrk+3jYbI5Mn84rLkxDb/lBGWB12JVY8z7IVeQMfVSy965YZa3m4fKN8n+S2mr+5RGy6RIn6M45Vxzqw=
---

# 实战排障记录

全部条目都是真机上踩出来的，按"症状 → 判据 → 根因 → 处置"写。不要凭猜测动手，先看判据。

---

## 1. 注入器报成功，但什么都没发生

**症状**：注入器退出码 0、打印句柄，`/proc/<pid>/maps` 里 `grep -c libesp.so` 结果为 0；目标进程不崩、不重启，就是不出菜单。

**判据**：

```bash
adb shell su 0 sh -c 'grep -c libesp_only.so /proc/$(pidof <包名>)/maps'
# 期望 3（x86_64 通常 3 段：r--p / r-xp / r--p），得到 0 就是没映射
```

**根因**：注入器 `RemoteCall` 里少减了 8 字节，`movaps` 撞栈破坏 16 字节栈对齐 → 目标线程执行时静默失败，返回假句柄。

**处置**：只用 `prebuilt/injector_x64`（**12,816 B**，md5 `21da0af9165c9a04b0c8d00edfa4459e`）。另一份 17,328 B 的版本必挂。这类问题**编译期完全看不出来**，只能真机冒烟。

---

## 2. JS 写端与 C++ 读端的 stride 不一致（对，就是我踩过的坑）

**症状**：第 1 个实体画得完全正确，从第 2 个实体开始方框乱飞、名字错位、`camp` 全是垃圾值。

**判据**：比对两端常量。

```bash
grep -n 'ENT_SZ=' js/esp_shm_block.js                 # 必须是 360
grep -n 'static_assert(sizeof(ShmEntity)' jni/src/shm_abi.h  # 必须是 360
```

**根因**：历史版本 JS 写的是 `ENT_SZ=356, HDR_SZ=22896`，即"字段长度相加"，漏掉了 8 字节对齐带来的填充（`id` 后 4 B + 结构体尾 4 B）。C++ `sizeof(ShmEntity)` 实际是 **360**。于是第 i 个实体写端算 `112 + i*356`，读端算 `112 + i*360`，每前进一个实体就漂移 4 字节。

**处置**：已修正为 `HDR_SZ=23152, ENT0=112, ENT_SZ=360`。并给 `shm_abi.h` 加了一组 `static_assert` 锁死 88 / 360 / 348 / 23152 / 460 与关键偏移，以后改结构体编译期直接炸。

**教训**：结构体大小一律 `sizeof()` 或离线 ctypes 复算，**不许手算**。掉填充的验算脚本见 [`docs/ABI.md` §9](ABI.md#9-一致性自检不编译也能验)。

---

## 3. so 架构和设备 ABI 看着一致，注入还是失败

**症状**：`getprop ro.product.cpu.abi` = `x86_64`，推了 x86_64 的 so 进不去；或者反过来，推 arm64 的 so 注入成功但 `dlopen` 失败。

**根因**：雷电/MuMu 这类模拟器的 `ro.product.cpu.abi` 反映的是**系统属性**，而游戏进程 `libc` 可能来自 houdini 转译层（`/system/vendor/lib64/arm64/libc.so`）——属性系统属性说的是 x86_64，真正的 ELF 机器码要自己读。

**判据**：只认 `e_machine`（ELF 头第 18–19 字节）：

```bash
od -An -tx1 -N2 -j18 libesp_x86_64.so     # 3e 00 = x86_64
od -An -tx1 -N2 -j18 libesp_arm64-v8a.so  # b7 00 = aarch64
```

`esp.sh status` 子命令已内置这条自检，不匹配直接 WARN。

---

## 4. 别拿文件大小判断 so 版本

**症状**：同一个 commit 编出来两次，`libesp_only.so` 一次 1,407,936 B、一次 1,292,680 B，怀疑编错了。

**根因**：LTO / `--gc-sections` / 符号表与调试信息差异都会让体积变。体积差不等于 ABI 差。

**处置**：版本判据只用 `md5` + `maps` 双证，两者都对才算部署成功。

---

## 5. 读端 mmap 文件，游戏整体崩进 SIGBUS

**症状**：注入后隔几秒进程直接退出，logcat 有 `SIGBUS` / `bus error`。

**根因**：JS 侧 `native.fileUtils.writeDataToFile` 等价 `fopen("wb")`，**先把文件截断到 0**再写。此刻 reader 若持着上一轮的 `mmap` 映射并访问越界页，直接 `SIGBUS`。

**处置**：v4 起读端改为 `open + fstat + read()` 全量快照，每帧重新读，绝不长期持有映射。不要再"优化"回 mmap。

---

## 6. 数据文件一直不刷新 / `[shm] path=` 打的路径不对

**判据**：

```bash
adb shell su 0 stat -c '%s %y' /data/local/tmp/esp_data.bin    # mtime 必须每帧变化
adb logcat -d -s esp:V | grep '\[shm\] path='
```

**根因**：JS 写端用 `native.fileUtils.getWritablePath()`，落到 `/data/user/0/<pkg>/files/`；native 读端默认读 `/data/local/tmp/esp_data.bin`（脚本传的路径）。两边不是同一个文件，读端就永远读不到。

**处置**：统一路径。脚本版把 JS 的 `path` 配置改成 `/data/local/tmp/esp_data.bin`，或把读端路径指到 `<writablePath>/esp_data.bin`。`esp.sh status` 会同时打出两边路径，先对一遍。

---

## 7. `-lGLESv3` 被删掉后链接/运行报未定义符号

**症状**：`ndk-build` 能过，运行时 `dlopen` 失败，logcat 报 `cannot locate symbol "glDrawElements"` 之类。

**根因**：`APP_PLATFORM := android-24` 下 GLES3 的 lib 文件名是 `libGLESv3.so`，`Android.mk` 必须显式链 `-lGLESv3`（ImGui 的 `imgui_impl_opengl3` 用的是 ES3 路径）。只写 `-lGLESv2` 不够。

**处置**：保持 `LOCAL_LDLIBS := -llog -landroid -lEGL -lGLESv3 -lGLESv2 -ldl -lm` 原样。

---

## 8. 触摸接管只认第一根手指

**症状**：模拟器里点面板要按两次；一边走路一边开菜单不可能；多点触控完全失效。

**根因**：期望 hook 的 `Java_com_cocos_lib_CocosTouchHandler_handleActionDown/Move/Up/Cancel` 四个符号在目标里**全不存在**（版本差异，符号被混淆）。回落到 `MotionEvent` 通道可行，但**只支持 `pointerId = 0`**。

**处置**：接受限制。面板交互与移动操作无法同时成立；需要边跑边开面板时用键盘映射的多键并发。

---

## 9. 方框上下颠倒 / 位置整体偏移

**根因**：Cocos 的 `worldToScreen` 是**左下原点、y 向上**，而 ImGui / 屏幕坐标是**左上原点、y 向下**。必须 `y = 1 - y` 翻转。

另外 `cc.UITransform` 不在 `cc` 全局对象上，直接 `cc.UITransform` 是 `undefined`，必须：

```js
node.addComponent("cc.UITransform")
```

**判据**：让实体站在屏幕正中央，方框应恰好覆盖人物；若方框出现在中线镜像位置，就是漏翻 y。

---

## 10. 分辨率 / 横竖屏切换后方框错位

**根因**：`surface` 尺寸变了，读端还用旧视口算像素。

**处置**：`pickCam()` 每 30 帧校验一次相机与 surface 变化并重建 ImGui 层；切换后允许最多 30 帧（约 0.5 s）错位。若长期不错位，说明相机校验没生效，检查 `hook.cpp` 里 `eglSwapBuffers` 的 surface 宽高取值是否在读缓存。

---

## 11. 中文面板显示方块

**根因**：字体是内置中文子集，新增字符不在子集里。

**处置**：改 `entry.cpp` 的字形范围（`io.Fonts->AddFontFromMemoryCompressedTTF` 的 `glyph_ranges`），或换带完整 CJK 的 TTF 并重新裁剪子集。注意体积：完整 CJK 字库会顶到几 MB。

---

## 12. 换 index.js 后游戏启动白屏 / 权限被拒

**根因**：写回 `index.js` 后权限、SELinux 上下文丢了。

**处置**：

```bash
chmod 600 <pkg>/files/.../index.js
restorecon -v <path>
chcon u:object_r:app_data_file:s0 <path>
```

`esp.sh` 换 JS 分支已内置这三步 + 备份到 `index.js.bak_pre_v6`；出问题直接 `esp.sh rollback`。

---

## 13. 注入幂等性

**症状**：反复跑注入，进程越来越卡，`maps` 里同名 so 出现多份。

**根因**：重复 `dlopen` / 重复 `pthread_create`。

**处置**：`esp.sh` 注入前先查 `/proc/<pid>/maps`，已映射则跳过。手动注入时也必须自查；库入口 `init` 用 `pthread_once` 兜底。

---

## 14. 一键体检

```bash
adb shell su 0 sh /data/local/tmp/imgui/esp.sh status
```

输出应包含：`device abi=`、`so machine=`（两者一致）、游戏 `pid=`、三件套文件存在性与 chmod、`esp_data.bin` 大小与 mtime。

`maps` 段数、日志链、mtime 三条全绿再谈功能问题，否则都是部署问题。
*（内容由AI生成，仅供参考）*

## 15. 真机上 arm64 进程不能用 x86_64 注入器

`injector_x64` 在真机上连启动都过不去（没有 houdini 转译层），即使强行跑起来，
`PTRACE_GETREGSET(NT_PRSTATUS)` 也会拿到 arm64 的 272 字节 `user_pt_regs`，
用 x86_64 的 `user_regs_struct`（216 字节的 rax/rsp/rip 布局）去解析，等于读了一堆垃圾。

判据：`od -An -tx1 -N2 -j18 inj_a64` → `b700`（aarch64）。拿不到 `b700` 就是编错了。
`injector/Android.mk` 已按 ABI 条件构建，不会给你一个"能跑但乱写寄存器"的产物。

## 16. `is not accessible for the namespace "classloader-namespace"`

真机 Android 9+ 的 linker namespace 只允许 App 从自己的 native lib 目录加载 so。
`/data/local/tmp`、`/data/data/<pkg>/files` 之类的路径一律被拒，报错就打在 logcat 里。

正确做法：so 落到 `/data/app/~~xxxx==/<pkg>-yyyy==/lib/arm64/`，再用**绝对路径** dlopen。
注意这一段带随机 hash，写死路径换台机器就废；脚本从 `/proc/<pid>/maps` 反查真实 lib 目录。

## 17. SELinux 拦在 cp 之后、dlopen 之前

`/data/local/tmp` 的 context 是 `shell_data_file`，app 域读不到；拷进 lib 目录时如果不做
`restorecon`，`cp` 保留的 context 会让 so 变成"看得见、load 不了"。

```bash
chmod 644 $LIBDIR/libesp_a64.so
restorecon $LIBDIR/libesp_a64.so      # 必须有，别只 chmod
ls -lZ $LIBDIR/libesp_a64.so          # 期望 u:object_r:apk_data_file:s0
```

排查时临时 `setenforce 0` 能快速区分是 SELinux 还是别的原因，但别长期开着。

## 18. KernelSU 下的 su 姿势

- `su -c "cmd"`：KSU 与 Magisk 都吃这一套，脚本内部提权用的就是它；
- `su 0 sh xxx.sh`：老 Magisk 习惯写法，KSU 某些版本不认第 1 个位置参数，别混用；
- KSU 默认不隔离 mount namespace，`/data/app`、`/data/user/0` 都是可写视图，直接 cp 即可；
  若开了"隔离挂载"，要在 Manager 里关掉对应 app 的隔离，否则 cp 进去的 so 目标进程看不到。

## 19. 真机注入后没有任何日志 ≠ 注入失败

先分清三层：
1. 二进制层：`grep -c libesp_a64.so /proc/<pid>/maps` = 3
2. 权限层：logcat 有 `avc: denied` / `not accessible for the namespace`
3. 业务层：`hook()` 是否打印、`GL_VERSION` 是否出来

段数到了 3 但日志停在 `esp.so 已加载`，通常是 `And64InlineHook` 没在 arm64 目标函数上命中
（真机 so 版本差异导致偏移变了），此时看 `hook(x64)`/`hook(arm64)` 那一行打的是哪条分支。

## 20. 真机上先别急着在大厅注入

大厅阶段 `cc::Scene` / `Camera` 全为 0，`pickCam()` 返回空，层建不起来也**不会报错**，
容易误判成"注入失败"。进训练场/对局后再 inject，或依赖模块内每 30 帧的 `pickCam()` 复检自动补建。
