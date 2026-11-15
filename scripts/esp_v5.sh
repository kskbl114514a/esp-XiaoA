#!/system/bin/sh
# ==========================================================================
#  esp_v5.sh  ---  ESP v5 一体化脚本（部署 / 注入 / 体检 / 回滚 四合一）
#  单文件，无外部依赖，toybox sh 兼容（Android 7+）
#
#  用法:
#    su 0 sh /data/local/tmp/imgui/esp_v5.sh              # 默认 all：换JS+冷启动+注入+校验
#    su 0 sh /data/local/tmp/imgui/esp_v5.sh all
#    su 0 sh /data/local/tmp/imgui/esp_v5.sh inject       # 只注入当前进程
#    su 0 sh /data/local/tmp/imgui/esp_v5.sh verify       # 只体检（只读）
#    su 0 sh /data/local/tmp/imgui/esp_v5.sh rollback     # 回滚 index.js + force-stop
#    su 0 sh /data/local/tmp/imgui/esp_v5.sh status       # 列出三件套文件状态
#    su 0 sh /data/local/tmp/imgui/esp_v5.sh all nojs     # 全套但不换 index.js
#
#  前置（手工放好即可）:
#    /data/local/tmp/imgui/libesp_only.so   按设备 CPU ABI 选的 so（x86 用 x86_64 版）
#    /data/local/tmp/imgui/inj_a            aim-esp 版 injector_x64（12,816B，别用 cheatkit 版）
#    /data/local/tmp/imgui/index_v6.js      新的热更 JS（可选；不换 JS 时不需要）
#    /data/local/tmp/imgui/index_v3.js      干净基线，rollback 用
# ==========================================================================

PKG=com.fmgame.pubg.aligames
ACT=com.cocos.game.AppActivity
GAME=/data/user/0/$PKG/files/pubgremote-asset/assets/game
SO=/data/local/tmp/imgui/libesp_only.so
INJ=/data/local/tmp/imgui/inj_a
JS_NEW=/data/local/tmp/imgui/index_v6.js
JS_BASE=/data/local/tmp/imgui/index_v3.js

CMD="$1"
OPT="$2"
say()  { echo "[esp] $*"; }
die()  { echo "[esp][FAIL] $*"; exit 1; }

# ---------- 设备 ABI 自检（防止推错 so） ----------
check_abi() {
  ABI=$(getprop ro.product.cpu.abi)
  MAGIC=$(od -An -tx1 -N2 -j18 "$SO" 2>/dev/null | tr -d ' \n')
  case "$MAGIC" in
    3e00) SOABI=x86_64 ;;
    b700) SOABI=aarch64 ;;
    *)    SOABI=unknown ;;
  esac
  say "device abi=$ABI  so machine=$SOABI"
  case "$ABI" in
    x86*)   [ "$SOABI" = "x86_64" ]  || say "WARN: so 不是 x86_64，雷电上会崩，换 libesp_x86_64.so" ;;
    arm64*) [ "$SOABI" = "aarch64" ] || say "WARN: so 不是 arm64，真机上会崩，换 libesp_arm64-v8a.so" ;;
  esac
}

# ---------- 前置文件检查 ----------
check_files() {
  [ -f "$SO" ]  || die "缺少 $SO"
  [ -f "$INJ" ] || die "缺少 $INJ"
  [ -x "$INJ" ] || chmod 755 "$INJ" 2>/dev/null
  [ -x "$INJ" ] || die "$INJ 不可执行"
}

# ---------- 注入 ----------
do_inject() {
  PID="$1"
  [ -z "$PID" ] && PID=$(pidof $PKG)
  [ -z "$PID" ] && die "$PKG 未运行，先冷启动（或跑 all）"

  PRE=$(grep -c "$SO" /proc/$PID/maps 2>/dev/null)
  if [ "$PRE" != "0" ]; then
    say "已映射 $PRE 段，跳过注入（要重来先 force-stop）"
    return 0
  fi

  say "注入 $SO -> pid $PID"
  logcat -c
  "$INJ" "$PID" "$SO" 2>&1 | tail -8
  sleep 6

  MAPS=$(grep -c "$SO" /proc/$PID/maps 2>/dev/null)
  say "maps 段数 = $MAPS （应为 3）"
  grep "$SO" /proc/$PID/maps 2>/dev/null | head -5

  if [ -z "$MAPS" ] || [ "$MAPS" = "0" ]; then
    logcat -d -s esp:V | tail -20
    die "so 未映射 => 注入器返回假句柄，换 aim-esp 版 injector_x64"
  fi

  say "关键日志:"
  logcat -d -s esp:V | grep -E "esp\.so|hook\(|GL_VERSION|ImGui|surface|\[shm\]|Perf" | tail -20

  if logcat -d -s esp:V | grep -q "ImGui"; then
    say "PASS  注入成功，ImGui 就绪，maps=$MAPS"
  else
    say "WARN  已映射但暂无 ImGui 日志，等 3s 后再跑 verify"
  fi
  return 0
}

# ---------- 换 JS ----------
do_swap_js() {
  [ -f "$JS_NEW" ] || { say "无 $JS_NEW，跳过换 JS"; return 0; }
  say "换 index.js（先备份 index.js.bak_pre_v6）"
  if [ -f "$GAME/index.js" ]; then
    cp -f "$GAME/index.js" "$GAME/index.js.bak_pre_v6"
    OWN=$(stat -c %u "$GAME/index.js.bak_pre_v6" 2>/dev/null)
    GRP=$(stat -c %g "$GAME/index.js.bak_pre_v6" 2>/dev/null)
  fi
  cp -f "$JS_NEW" "$GAME/index.js" || die "写入 $GAME/index.js 失败"
  [ -n "$OWN" ] && chown "$OWN:$GRP" "$GAME/index.js" 2>/dev/null
  chmod 600 "$GAME/index.js"
  restorecon "$GAME/index.js" 2>/dev/null
  chcon u:object_r:app_data_file:s0 "$GAME/index.js" 2>/dev/null
  md5sum "$GAME/index.js" "$JS_NEW" 2>/dev/null
  ls -l "$GAME/index.js"
}

# ---------- 冷启动 ----------
do_start() {
  say "force-stop"
  am force-stop $PKG
  sleep 2
  say "am start"
  am start -n $PKG/$ACT 2>&1 | tail -2
  say "等待 pid（最多 60s）"
  i=0
  while [ $i -lt 60 ]; do
    PID=$(pidof $PKG)
    [ -n "$PID" ] && break
    sleep 1
    i=$((i+1))
  done
  [ -z "$PID" ] && die "60s 内没有进程"
  say "pid=$PID"
  say "等 10s 让 cocos 起来"
  sleep 10
}

# ---------- 体检 ----------
do_verify() {
  PID=$(pidof $PKG)
  say "pid = ${PID:-未运行}"
  [ -z "$PID" ] && return 1

  say "--- so 映射 ---"
  grep "$SO" /proc/$PID/maps 2>/dev/null | head -5
  say "   段数: $(grep -c "$SO" /proc/$PID/maps 2>/dev/null)"

  say "--- esp 线程 ---"
  for t in /proc/$PID/task/*; do
    n=$(cat $t/comm 2>/dev/null)
    case "$n" in *esp*|*Esp*|*ensor*) say "   $n" ;; esac
  done
  say "   线程总数: $(ls /proc/$PID/task | wc -l)"

  say "--- 关键日志 ---"
  logcat -d -s esp:V | grep -E "hook\(|GL_VERSION|ImGui|surface|\[shm\]|Perf" | tail -12

  say "--- esp_data.bin（隔 2s 看是否在刷新）---"
  BIN=$(logcat -d -s esp:V | grep -o "/[^ ]*esp_data.bin" | tail -1)
  [ -z "$BIN" ] && BIN=/data/local/tmp/esp_data.bin
  if [ -f "$BIN" ]; then
    say "   path: $BIN"
    stat -c "   t0 size=%s mtime=%y" "$BIN" 2>/dev/null
    sleep 2
    stat -c "   t1 size=%s mtime=%y" "$BIN" 2>/dev/null
    say "   （两次 mtime 不同 = JS 正在写；不变 = JS 侧没起来）"
  else
    say "   未找到 $BIN（JS 未写盘 / 路径不同，看 logcat | grep shm）"
  fi
  return 0
}

# ---------- 回滚 ----------
do_rollback() {
  say "force-stop"
  am force-stop $PKG
  sleep 2
  if [ -f "$JS_BASE" ]; then
    say "恢复 $JS_BASE -> index.js"
    [ -f "$GAME/index.js" ] && cp -f "$GAME/index.js" "$GAME/index.js.bak_before_rollback"
    cp -f "$JS_BASE" "$GAME/index.js"
    OWN=$(stat -c %u "$GAME" 2>/dev/null); GRP=$(stat -c %g "$GAME" 2>/dev/null)
    [ -n "$OWN" ] && chown "$OWN:$GRP" "$GAME/index.js" 2>/dev/null
    chmod 600 "$GAME/index.js"
    restorecon "$GAME/index.js" 2>/dev/null
    md5sum "$GAME/index.js" "$JS_BASE" 2>/dev/null
  else
    say "没有基线 $JS_BASE，只做了 force-stop"
  fi
  say "回滚完成，pid=$(pidof $PKG)"
}

# ---------- 三件套状态 ----------
do_status() {
  for f in "$SO" "$INJ" "$JS_NEW" "$JS_BASE"; do
    if [ -f "$f" ]; then
      say "$(ls -l "$f" | awk '{print $1, $5, $NF}')  md5=$(md5sum "$f" 2>/dev/null | cut -c1-32)"
    else
      say "MISSING $f"
    fi
  done
  check_abi
  say "game pid = $(pidof $PKG)"
}

# ========================== main ==========================
case "$CMD" in
  ""|all|deploy)
    check_files
    check_abi
    if [ "$OPT" != "nojs" ]; then do_swap_js; else say "nojs 模式，不换 JS"; fi
    do_start
    do_inject "$PID"
    echo
    do_verify
    ;;
  inject)
    check_files
    check_abi
    do_inject "$OPT"
    ;;
  verify)
    do_verify
    ;;
  rollback)
    do_rollback
    ;;
  status)
    do_status
    ;;
  *)
    echo "用法: sh $0 {all|inject|verify|rollback|status} [nojs|pid]"
    exit 2
    ;;
esac
