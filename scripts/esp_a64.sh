#!/system/bin/sh
# ==========================================================================
#  esp_a64.sh --- 真机 arm64 一体化脚本（部署 / 注入 / 体检 / 回滚）
#  面向: 真机 arm64-v8a + root(KernelSU/Magisk) + 非 houdini 环境
#  与 esp_v5.sh 的区别:
#    1. 用 arm64 版注入器 inj_a64（不用 x64 注入器，真机上 arm64 进程必须 arm64 注入器）
#    2. so 必须先 stage 到 App 的 native lib 目录 —— /data/local/tmp 下的 so
#       在真机上会被 linker namespace 拒绝（"not accessible for the namespace"）
#       且 SELinux 也会拦（shell_data_file 只属 shell）
#    3. 校验对象为 libesp_a64.so，maps 段数仍应为 3
#
#  用法（root shell 内）:
#    su -c "sh /data/local/tmp/imgui/esp_a64.sh status"
#    su -c "sh /data/local/tmp/imgui/esp_a64.sh stage"      # 只把 so 铺到 lib 目录
#    su -c "sh /data/local/tmp/imgui/esp_a64.sh inject"     # 注入当前 pid（先 force-stop 才干净）
#    su -c "sh /data/local/tmp/imgui/esp_a64.sh verify"
#    su -c "sh /data/local/tmp/imgui/esp_a64.sh rollback"
#    su -c "sh /data/local/tmp/imgui/esp_a64.sh all auto"   # 换JS+冷启动+等 45s+自动注入
#    su -c "sh /data/local/tmp/imgui/esp_a64.sh all nojs"   # 不换 JS
#
#  前置文件（推到 /data/local/tmp/imgui/）:
#    inj_a64                11,080 B  aarch64 PIE 注入器
#    libesp_arm64.so        1,292,680 B  arm64-v8a 模块（仓库 prebuilt/libesp_arm64-v8a.so 改名）
#    index_v6.js            新热更 JS（可选）
#    index_v3.js            干净基线（rollback 用，可选）
#
#  真机时序铁律: 大厅里 Scene/Camera 为 0，必须进对局/训练场后再 inject
# ==========================================================================

PKG=com.fmgame.pubg.aligames
DIR=/data/local/tmp/imgui
INJ=$DIR/inj_a64
SO_SRC=$DIR/libesp_arm64.so
SO_NAME=libesp_a64.so               # 落到 lib 目录后用这个名字，避免和原库重名
JS_NEW=$DIR/index_v6.js
JS_BASE=$DIR/index_v3.js

CMD="$1"
OPT="$2"
say() { echo "[esp-a64] $*"; }
die() { echo "[esp-a64][FAIL] $*"; exit 1; }

# ---------- root 自提权（KernelSU: su -c；Magisk: su -c；两者都兼容） ----------
CMDLINE="$*"
ensure_root() {
  [ "$(id -u)" = "0" ] && return 0
  say "非 root，尝试 su 自提权"
  exec su -c "sh $0 $CMDLINE"
}

# ---------- 设备/文件架构自检 ----------
machine_of() {
  od -An -tx1 -N2 -j18 "$1" 2>/dev/null | tr -d ' \n'
}
check_abi() {
  ABI=$(getprop ro.product.cpu.abi)
  EMSO=$(machine_of "$SO_SRC"); EMIN=$(machine_of "$INJ")
  case "$EMSO" in b700) SOABI=aarch64 ;; 3e00) SOABI=x86_64 ;; *) SOABI=unknown ;; esac
  case "$EMIN" in b700) INABI=aarch64 ;; 3e00) INABI=x86_64 ;; *) INABI=unknown ;; esac
  say "device-abi=$ABI  so=$SOABI  inj=$INABI"
  case "$ABI" in
    arm64*)
      [ "$SOABI" = aarch64 ] || die "so 不是 aarch64，真机会崩（换 prebuilt/libesp_arm64-v8a.so）"
      [ "$INABI" = aarch64 ] || die "注入器不是 aarch64，真机跑不起来（换 prebuilt/arm64/inj_a64）"
      ;;
    *) die "本机 abi=$ABI，不是 arm64 真机；模拟器请用 esp_v5.sh + injector_x64" ;;
  esac
}

check_files() {
  [ -f "$SO_SRC" ] || die "缺少 $SO_SRC"
  [ -f "$INJ" ]    || die "缺少 $INJ"
  chmod 755 "$INJ" 2>/dev/null
  [ -x "$INJ" ]    || die "$INJ 不可执行"
}

# ---------- 取 App native lib 目录（优先从运行中进程 maps 探测） ----------
get_libdir() {
  PID=$(pidof $PKG)
  if [ -n "$PID" ]; then
    L=$(grep -m1 '/lib/arm64/' /proc/$PID/maps 2>/dev/null | awk '{print $6}')
    [ -n "$L" ] && { dirname "$L"; return 0; }
  fi
  P=$(pm path $PKG 2>/dev/null | head -1 | sed 's/^package://')
  [ -n "$P" ] && { echo "$(dirname "$P")/lib/arm64"; return 0; }
  return 1
}

# ---------- 把 so 铺到 native lib 目录（绕 linker namespace + SELinux） ----------
do_stage() {
  check_files
  LIBDIR=$(get_libdir) || die "拿不到 lib 目录（进程没起且 pm path 失败）"
  say "lib 目录: $LIBDIR"
  [ -d "$LIBDIR" ] || mkdir -p "$LIBDIR" || die "创建 $LIBDIR 失败"
  REF=$(ls "$LIBDIR" 2>/dev/null | head -1)
  cp -f "$SO_SRC" "$LIBDIR/$SO_NAME" || die "写入 $LIBDIR/$SO_NAME 失败"
  if [ -n "$REF" ]; then
    OW=$(stat -c %u "$LIBDIR/$REF" 2>/dev/null); GR=$(stat -c %g "$LIBDIR/$REF" 2>/dev/null)
    [ -n "$OW" ] && chown "$OW:$GR" "$LIBDIR/$SO_NAME" 2>/dev/null
  fi
  chmod 644 "$LIBDIR/$SO_NAME" 2>/dev/null
  restorecon "$LIBDIR/$SO_NAME" 2>/dev/null
  ls -lZ "$LIBDIR/$SO_NAME" 2>/dev/null || ls -l "$LIBDIR/$SO_NAME"
  SO_TARGET="$LIBDIR/$SO_NAME"
  say "stage 完成 -> $SO_TARGET"
}

# ---------- 注入 ----------
do_inject() {
  check_files
  check_abi
  PID="$1"; [ -z "$PID" ] && PID=$(pidof $PKG)
  [ -z "$PID" ] && die "$PKG 未运行"

  # 注入前确保 so 已在 lib 目录（每台设备 lib 路径带随机 hash，不能写死）
  do_stage >/dev/null
  say "目标 pid=$PID  so=$SO_TARGET"

  PRE=$(grep -c "$SO_NAME" /proc/$PID/maps 2>/dev/null)
  if [ -n "$PRE" ] && [ "$PRE" != "0" ]; then
    say "已映射 $PRE 段，跳过注入（重来先 force-stop）"
    return 0
  fi

  logcat -c 2>/dev/null
  "$INJ" "$PID" "$SO_TARGET" 2>&1 | tail -12
  sleep 5
  MAPS=$(grep -c "$SO_NAME" /proc/$PID/maps 2>/dev/null)
  say "maps 段数 = ${MAPS:-0} （应为 3）"
  grep "$SO_NAME" /proc/$PID/maps 2>/dev/null | head -4

  if [ -z "$MAPS" ] || [ "$MAPS" = "0" ]; then
    say "--- 诊断 ---"
    logcat -d 2>/dev/null | grep -E "not accessible for the namespace|avc: *denied|dlopen|cannot locate" | tail -8
    logcat -d -s esp:V 2>/dev/null | tail -10
    die "so 未映射，按上面诊断处理（namespace 拦截 -> 检查 stage 路径；avc denied -> restorecon/降 permissive）"
  fi
  say "--- 关键日志 ---"
  logcat -d -s esp:V 2>/dev/null | grep -E "esp\.so|hook\(|GL_VERSION|ImGui|surface|\[shm\]|Perf" | tail -15
  return 0
}

# ---------- 换 JS ----------
GAMEDIR() { echo "/data/user/0/$PKG/files/pubgremote-asset/assets/game"; }
do_swap_js() {
  G=$(GAMEDIR)
  [ -f "$JS_NEW" ] || { say "无 $JS_NEW，跳过换 JS"; return 0; }
  [ -d "$G" ] || { say "无 $G，跳过换 JS（先冷启动一次）"; return 0; }
  [ -f "$G/index.js" ] && cp -f "$G/index.js" "$G/index.js.bak_pre_v6"
  OW=$(stat -c %u "$G" 2>/dev/null); GR=$(stat -c %g "$G" 2>/dev/null)
  cp -f "$JS_NEW" "$G/index.js" || die "写 $G/index.js 失败"
  [ -n "$OW" ] && chown "$OW:$GR" "$G/index.js" 2>/dev/null
  chmod 600 "$G/index.js"
  restorecon "$G/index.js" 2>/dev/null
  chcon u:object_r:app_data_file:s0 "$G/index.js" 2>/dev/null
  md5sum "$G/index.js" "$JS_NEW" 2>/dev/null
}

# ---------- 冷启动 ----------
get_act() {
  A=$(cmd package resolve-activity --brief $PKG 2>/dev/null | tail -1)
  case "$A" in */*) echo "$A" ;; *) echo "$PKG/com.cocos.game.AppActivity" ;; esac
}
do_start() {
  am force-stop $PKG; sleep 2
  ACT=$(get_act)
  say "am start $ACT"
  am start -n "$ACT" 2>&1 | tail -2
  i=0
  while [ $i -lt 60 ]; do PID=$(pidof $PKG); [ -n "$PID" ] && break; sleep 1; i=$((i+1)); done
  [ -z "$PID" ] && die "60s 内没进程"
  say "pid=$PID，等 12s 让引擎起来"; sleep 12
}

# ---------- 体检 ----------
do_verify() {
  PID=$(pidof $PKG)
  say "pid = ${PID:-未运行}"; [ -z "$PID" ] && return 1
  say "--- so 映射（应 3 段）---"
  grep "$SO_NAME" /proc/$PID/maps 2>/dev/null | head -5
  say "   段数: $(grep -c "$SO_NAME" /proc/$PID/maps 2>/dev/null)"
  say "--- esp 线程 ---"
  for t in /proc/$PID/task/*; do
    n=$(cat $t/comm 2>/dev/null)
    case "$n" in *esp*|*Esp*|*ensor*) say "   $n" ;; esac
  done
  say "   线程总数: $(ls /proc/$PID/task | wc -l)"
  say "--- 关键日志 ---"
  logcat -d -s esp:V 2>/dev/null | grep -E "hook\(|GL_VERSION|ImGui|surface|\[shm\]|Perf" | tail -12
  say "--- esp_data.bin ---"
  BIN=$(logcat -d -s esp:V 2>/dev/null | grep -o "/[^ ]*esp_data.bin" | tail -1)
  [ -z "$BIN" ] && BIN=/data/local/tmp/esp_data.bin
  if [ -f "$BIN" ]; then
    say "   $BIN"; stat -c "   t0 size=%s mtime=%y" "$BIN" 2>/dev/null; sleep 2
    stat -c "   t1 size=%s mtime=%y" "$BIN" 2>/dev/null
    say "   （mtime 变化 = JS 在写）"
  else
    say "   未找到 $BIN（JS 未起 / 路径不同）"
  fi
  return 0
}

# ---------- 回滚 ----------
do_rollback() {
  am force-stop $PKG; sleep 2
  G=$(GAMEDIR)
  if [ -f "$JS_BASE" ] && [ -d "$G" ]; then
    cp -f "$JS_BASE" "$G/index.js"
    OW=$(stat -c %u "$G" 2>/dev/null); GR=$(stat -c %g "$G" 2>/dev/null)
    [ -n "$OW" ] && chown "$OW:$GR" "$G/index.js" 2>/dev/null
    chmod 600 "$G/index.js"; restorecon "$G/index.js" 2>/dev/null
    md5sum "$G/index.js" "$JS_BASE" 2>/dev/null
  else
    say "无基线 $JS_BASE，仅 force-stop"
  fi
  LIBDIR=$(get_libdir) && rm -f "$LIBDIR/$SO_NAME" 2>/dev/null
  say "回滚完成 pid=$(pidof $PKG)"
}

# ---------- 状态 ----------
do_status() {
  say "root=$(id -u)  pkg=$PKG  pid=$(pidof $PKG)"
  for f in "$INJ" "$SO_SRC" "$JS_NEW" "$JS_BASE"; do
    if [ -f "$f" ]; then say "$(ls -l "$f" | awk '{print $1, $5, $NF}')  md5=$(md5sum "$f" 2>/dev/null | cut -c1-32)"
    else say "MISSING $f"; fi
  done
  LIBDIR=$(get_libdir) && say "libdir=$LIBDIR  staged=$([ -f "$LIBDIR/$SO_NAME" ] && echo yes || echo no)"
  check_abi
  say "SELinux: $(getenforce 2>/dev/null)"
}

# ========================== main ==========================
ensure_root
case "$CMD" in
  ""|all)
    check_files; check_abi
    if [ "$OPT" != "nojs" ]; then do_swap_js; else say "nojs 模式"; fi
    do_start
    if [ "$OPT" = "auto" ]; then
      say "auto 模式：等 45s 供手动进入对局/训练场，然后自动注入"
      sleep 45
      do_inject "$PID"
      do_verify
    else
      say "现在进对局/训练场，然后执行: sh $0 inject"
      say "（大厅里 Camera 为 0，注入拿不到相机；每 30 帧自检会自动补建层）"
    fi
    ;;
  stage)    do_stage ;;
  inject)   do_inject "$OPT" ;;
  verify)   do_verify ;;
  rollback) do_rollback ;;
  status)   do_status ;;
  *) echo "用法: sh $0 {all [auto|nojs]|stage|inject [pid]|verify|rollback|status}"; exit 2 ;;
esac
