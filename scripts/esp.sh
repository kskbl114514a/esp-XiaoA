#!/system/bin/sh
# ==========================================================================
#  esp.sh --- ESP 一体化单文件脚本
#  合并来源: esp_v5.sh(模拟器 x86_64) + esp_a64.sh(真机 arm64) + cheatkit/esp.sh(启停/日志)
#  单文件，无外部依赖，toybox sh 兼容（Android 7+），全 POSIX 写法
#  （不使用 bash 数组 / [[ ]] / {N..M}，mksh 与 toybox sh 均可跑）
#
#  运行时会按 ro.product.cpu.abi 自动选平台分支：
#    arm64*  -> ARM64 模式：so 必须先 stage 进 App 的 native lib 目录
#               (/data/app/~~xxx/<pkg>-yyy/lib/arm64/)，tmp 下的 so 会被
#               linker namespace 拒绝（not accessible for the namespace）
#    x86*    -> X64 模式：雷电/模拟器，so 放 /data/local/tmp/imgui 直接注入
#
#  用法（root shell 内，su -c 或 su 0 均可）:
#    su -c "sh /data/local/tmp/imgui/esp.sh"                 # 等价 all
#    su -c "sh /data/local/tmp/imgui/esp.sh all"             # 换JS+冷启动+注入+体检
#    su -c "sh /data/local/tmp/imgui/esp.sh all nojs"        # 全套但不换 index.js
#    su -c "sh /data/local/tmp/imgui/esp.sh all auto"        # 冷启动后等 45s 再自动注入
#    su -c "sh /data/local/tmp/imgui/esp.sh all auto nojs"   # 两者组合
#    su -c "sh /data/local/tmp/imgui/esp.sh deploy"          # 同 all（兼容旧习惯）
#    su -c "sh /data/local/tmp/imgui/esp.sh stage"           # 只把 so 铺进 lib 目录
#    su -c "sh /data/local/tmp/imgui/esp.sh start [秒]"      # 只冷启动，不注入
#    su -c "sh /data/local/tmp/imgui/esp.sh stop"            # force-stop
#    su -c "sh /data/local/tmp/imgui/esp.sh restart [秒]"    # stop + start + 注入
#    su 0 sh /data/local/tmp/imgui/esp.sh inject             # 只注入当前进程
#    su 0 sh /data/local/tmp/imgui/esp.sh inject 12345       # 指定 pid 注入
#    su 0 sh /data/local/tmp/imgui/esp.sh verify             # 只读体检
#    su 0 sh /data/local/tmp/imgui/esp.sh rollback           # 恢复基线 index.js + 卸载 so
#    su 0 sh /data/local/tmp/imgui/esp.sh status             # 文件/ABI/libdir/SELinux 全览
#    su 0 sh /data/local/tmp/imgui/esp.sh log                # 实时过滤引擎日志
#    su 0 sh /data/local/tmp/imgui/esp.sh cfg                # 查看悬浮窗配置
#
#  前置文件（统一推 /data/local/tmp/imgui/，脚本自动挑存在的那个）:
#    arm64 : inj_a64 (11,080B, aarch64 PIE) + libesp_arm64.so (1,292,680B)
#    x86_64: injector_x64 (12,816B, aim-esp 版) + libesp_x86_64.so (1,397,400B)
#    可选  : index_v6.js（新热更 JS） index_v3.js（干净基线，rollback 用）
#
#  验收铁律: /proc/<pid>/maps 中该 so 必须出现 3 次（EXPECT_MAPS），
#            注入器的文字回显（sig=11 等）不作为成功依据。
#  真机时序: 大厅里 Scene/Camera 为 0，必须进对局/训练场后再 inject。
#
#  环境变量覆盖（全部可选）:
#    ESP_PKG ESP_ACT ESP_DIR ESP_BOOT_WAIT ESP_AUTO_WAIT ESP_STAGE_NAME
#    ESP_GAME ESP_CFG ESP_EXPECT_MAPS
# ==========================================================================

PKG="${ESP_PKG:-com.fmgame.pubg.aligames}"
ACT_FB="${ESP_ACT:-com.cocos.game.AppActivity}"
DIR="${ESP_DIR:-/data/local/tmp/imgui}"
BOOT_WAIT="${ESP_BOOT_WAIT:-12}"
AUTO_WAIT="${ESP_AUTO_WAIT:-45}"
STAGE_NAME="${ESP_STAGE_NAME:-libesp_a64.so}"
GAME="${ESP_GAME:-/data/user/0/$PKG/files/pubgremote-asset/assets/game}"
CFG="${ESP_CFG:-/sdcard/Android/data/$PKG/files/esp_cfg.ini}"
EXPECT_MAPS="${ESP_EXPECT_MAPS:-3}"

SO_CANDS_ARM64="libesp_arm64.so libesp_arm64-v8a.so libesp_only.so libesp.so"
SO_CANDS_X64="libesp_x86_64.so libesp_only.so libesp.so"
INJ_CANDS_ARM64="inj_a64"
INJ_CANDS_X64="injector_x64 inj_a inj_x64"

CMD="${1:-}"
CMDLINE="$*"
NOJS=0
AUTO=0
for _a in "$@"; do
  case "$_a" in
    nojs) NOJS=1 ;;
    auto) AUTO=1 ;;
  esac
done

say()  { echo "[esp] $*"; }
err()  { echo "[esp][!] $*" >&2; }
die()  { echo "[esp][FAIL] $*" >&2; exit 1; }

# ---------------------------------------------------------------- 提权自举
ensure_root() {
  [ "$(id -u)" = "0" ] && return 0
  if command -v su >/dev/null 2>&1; then
    say "非 root，su 自提权: sh $0 $CMDLINE"
    exec su -c "sh $0 $CMDLINE"
  fi
  die "需要 root，且 su 不可用"
}

# ---------------------------------------------------------------- 平台探测
detect_platform() {
  ABI=$(getprop ro.product.cpu.abi 2>/dev/null)
  [ -z "$ABI" ] && ABI=$(getprop ro.product.cpu.abilist 2>/dev/null | cut -d, -f1)
  case "$ABI" in
    arm64*|aarch64*) PLAT=arm64 ;;
    x86_64*|x64*)    PLAT=x64 ;;
    x86*)            PLAT=x64 ;;
    *)               PLAT=unknown ;;
  esac
  if [ "$PLAT" = "unknown" ]; then
    # 再兜一层：看设备上有没有 arm64 的 so 需求，用 uname 判定
    case "$(uname -m 2>/dev/null)" in
      aarch64) PLAT=arm64; ABI=${ABI:-aarch64} ;;
      x86_64)  PLAT=x64;   ABI=${ABI:-x86_64} ;;
    esac
  fi
  return 0
}

# 在 $DIR 里挑第一个存在的候选文件
pick_first() {
  for _c in $1; do
    if [ -f "$DIR/$_c" ]; then
      echo "$DIR/$_c"
      return 0
    fi
  done
  return 1
}

resolve_bins() {
  detect_platform
  say "device-abi=$ABI  plat=$PLAT"
  case "$PLAT" in
    arm64)
      SO_SRC=$(pick_first "$SO_CANDS_ARM64")
      INJ=$(pick_first "$INJ_CANDS_ARM64")
      SO_NAME="$STAGE_NAME"
      [ -z "$SO_SRC" ] && err "缺少 arm64 so（$DIR 下没有: $SO_CANDS_ARM64）"
      [ -z "$INJ" ]    && err "缺少 arm64 注入器（$DIR/inj_a64 不存在）"
      ;;
    x64)
      SO_SRC=$(pick_first "$SO_CANDS_X64")
      INJ=$(pick_first "$INJ_CANDS_X64")
      [ -z "$SO_SRC" ] && err "缺少 x86_64 so（$DIR 下没有: $SO_CANDS_X64）"
      [ -z "$INJ" ]    && err "缺少 x64 注入器（$DIR 下没有: $INJ_CANDS_X64）"
      [ -n "$SO_SRC" ] && SO_NAME=$(basename "$SO_SRC")
      ;;
    *)
      err "无法识别平台（abi=${ABI:-空}），请用 ESP_PKG/ESP_DIR 显式指定或检查 getprop"
      ;;
  esac
  case "$1" in
    strict)
      [ -n "$SO_SRC" ] && [ -n "$INJ" ] || die "前置文件不全，无法继续（先 esp.sh status 看清单）"
      ;;
  esac
  SO_TARGET="$SO_SRC"
  return 0
}

# ---------------------------------------------------------------- 架构自检
machine_of() { od -An -tx1 -N2 -j18 "$1" 2>/dev/null | tr -d ' \n'; }

check_abi() {
  [ -f "$SO_SRC" ] || return 0
  EMSO=$(machine_of "$SO_SRC")
  EMIN=$(machine_of "$INJ")
  case "$EMSO" in b700) SOABI=aarch64 ;; 3e00) SOABI=x86_64 ;; 0300) SOABI=x86 ;; *) SOABI=unknown ;; esac
  case "$EMIN" in b700) INABI=aarch64 ;; 3e00) INABI=x86_64 ;; 0300) INABI=x86 ;; *) INABI=unknown ;; esac
  say "so=$(basename "$SO_SRC")[$SOABI]  inj=$(basename "$INJ")[$INABI]"
  case "$PLAT" in
    arm64)
      [ "$SOABI" = "aarch64" ] || die "so 不是 aarch64，真机会崩（换 prebuilt/libesp_arm64-v8a.so 改名 libesp_arm64.so）"
      [ "$INABI" = "aarch64" ] || die "注入器不是 aarch64，真机跑不起来（换 prebuilt/arm64/inj_a64）"
      ;;
    x64)
      if [ "$SOABI" = "aarch64" ]; then
        die "so 是 aarch64，模拟器上会崩（换 prebuilt/libesp_x86_64.so）"
      fi
      if [ "$INABI" = "aarch64" ]; then
        die "注入器是 aarch64，模拟器跑不了（换 prebuilt/injector_x64）"
      fi
      ;;
  esac
  return 0
}

check_files() {
  [ -n "$SO_SRC" ] || die "缺少 so（$DIR 下找不到适配本机 ABI 的 so，跑 status 看清单）"
  [ -n "$INJ" ]    || die "缺少注入器（$DIR 下找不到适配本机 ABI 的注入器）"
  [ -f "$SO_SRC" ] || die "缺少 so: $SO_SRC"
  [ -f "$INJ" ]    || die "缺少注入器: $INJ"
  chmod 755 "$INJ" 2>/dev/null
  [ -x "$INJ" ]    || die "$INJ 不可执行"
  return 0
}

# ---------------------------------------------------------------- 基础查询
getpid() { pidof "$PKG" 2>/dev/null | awk '{print $1}'; }

get_act() {
  _act=$(cmd package resolve-activity --brief "$PKG" 2>/dev/null | tail -1)
  case "$_act" in
    */*) echo "$_act" ;;
    *)   echo "$PKG/$ACT_FB" ;;
  esac
}

# 取 App native lib 目录：优先从运行中进程 maps 反查（路径含随机 hash，不能写死）
get_libdir() {
  _p=$(getpid)
  if [ -n "$_p" ]; then
    _l=$(grep -m1 '/lib/arm64' "/proc/$_p/maps" 2>/dev/null | awk '{print $6}')
    if [ -n "$_l" ]; then
      dirname "$_l"
      return 0
    fi
  fi
  _pp=$(pm path "$PKG" 2>/dev/null | head -1 | sed 's/^package://')
  if [ -n "$_pp" ]; then
    echo "$(dirname "$_pp")/lib/arm64"
    return 0
  fi
  return 1
}

# ---------------------------------------------------------------- stage
# arm64: 把 so 铺进 native lib 目录，绕 linker namespace 与 SELinux(shell_data_file)
# x64  : tmp 目录本身可注入，直接把 SO_TARGET 指向源文件
do_stage() {
  if [ "$PLAT" != "arm64" ]; then
    SO_TARGET="$SO_SRC"
    say "plat=$PLAT，无需 stage，so 直接用 $SO_TARGET"
    return 0
  fi
  check_files
  LIBDIR=$(get_libdir) || die "拿不到 lib 目录（进程没起且 pm path 失败）"
  say "lib 目录: $LIBDIR"
  [ -d "$LIBDIR" ] || mkdir -p "$LIBDIR" || die "创建 $LIBDIR 失败"
  _ref=$(ls "$LIBDIR" 2>/dev/null | head -1)
  cp -f "$SO_SRC" "$LIBDIR/$SO_NAME" || die "写入 $LIBDIR/$SO_NAME 失败"
  if [ -n "$_ref" ]; then
    _ow=$(stat -c %u "$LIBDIR/$_ref" 2>/dev/null)
    _gr=$(stat -c %g "$LIBDIR/$_ref" 2>/dev/null)
    [ -n "$_ow" ] && chown "$_ow:$_gr" "$LIBDIR/$SO_NAME" 2>/dev/null
  fi
  chmod 644 "$LIBDIR/$SO_NAME" 2>/dev/null
  restorecon "$LIBDIR/$SO_NAME" 2>/dev/null
  ls -lZ "$LIBDIR/$SO_NAME" 2>/dev/null || ls -l "$LIBDIR/$SO_NAME"
  SO_TARGET="$LIBDIR/$SO_NAME"
  say "stage 完成 -> $SO_TARGET"
  return 0
}

# ---------------------------------------------------------------- 输入设备权限
# 游戏进程以 app uid 运行，/dev/input/event* 默认 root:input 0660、/dev/uinput 默认 uhid:uhid 0660，
# 非 root 打不开 -> evdev 通道起不来 -> ImGui 收不到触摸（菜单点不动）。
# /dev 由 init 在每次开机/模拟器重启时重建，权限必然回滚，所以每次注入前自愈一次。
fix_input_perm() {
  _fixed=0
  if [ -e /dev/input/event0 ]; then
    chmod 666 /dev/input/event* 2>/dev/null && _fixed=1
  fi
  if [ -e /dev/uinput ]; then
    chmod 666 /dev/uinput 2>/dev/null && _fixed=1
  fi
  _ep=$(ls /dev/input/event* 2>/dev/null | head -1)
  if [ -n "$_ep" ]; then
    _pm=$(ls -l "$_ep" 2>/dev/null | cut -c1-10)
    _um=$(ls -l /dev/uinput 2>/dev/null | cut -c1-10)
    say "输入设备权限: ${_ep}=${_pm:-无}  /dev/uinput=${_um:-无}"
    case "$_pm" in
      crw-rw-rw-*) : ;;
      *) err "chmod /dev/input/event* 失败 -> 触摸通道会不可用（需 root 运行本脚本）" ;;
    esac
    case "$_um" in
      crw-rw-rw-*|"") : ;;
      *) err "chmod /dev/uinput 失败 -> 触摸自瞄/穿透不可用" ;;
    esac
  fi
  [ "$_fixed" = "1" ]
  return 0
}

# ---------------------------------------------------------------- 注入
do_inject() {
  fix_input_perm
  check_files
  check_abi
  do_stage >/dev/null

  PID="$1"
  [ -z "$PID" ] && PID=$(getpid)
  [ -z "$PID" ] && die "$PKG 未运行（先 start 或 all）"
  [ -d "/proc/$PID" ] || die "pid $PID 不存在"
  [ -z "$SO_TARGET" ] && SO_TARGET="$SO_SRC"

  say "目标 pid=$PID  uid=$(stat -c %U /proc/$PID 2>/dev/null)"
  say "注入 so: $SO_TARGET  注入器: $INJ"

  PRE=$(grep -c "$SO_NAME" "/proc/$PID/maps" 2>/dev/null)
  if [ -n "$PRE" ] && [ "$PRE" != "0" ]; then
    say "已映射 $PRE 段，跳过注入（要重来先 force-stop）"
    grep "$SO_NAME" "/proc/$PID/maps" 2>/dev/null | head -3
    return 0
  fi

  logcat -c 2>/dev/null
  "$INJ" "$PID" "$SO_TARGET" 2>&1 | tail -12
  sleep 5

  MAPS=$(grep -c "$SO_NAME" "/proc/$PID/maps" 2>/dev/null)
  say "maps 段数 = ${MAPS:-0}（应为 $EXPECT_MAPS）"
  grep "$SO_NAME" "/proc/$PID/maps" 2>/dev/null | head -5

  if [ -z "$MAPS" ] || [ "$MAPS" = "0" ]; then
    say "--- 诊断 ---"
    logcat -d 2>/dev/null | grep -E "not accessible for the namespace|avc: *denied|dlopen|cannot locate" | tail -8
    logcat -d -s esp:V 2>/dev/null | tail -10
    if [ "$PLAT" = "arm64" ]; then
      err "so 未映射。namespace 拦截 -> 查 stage 路径是否在 lib/arm64；avc denied -> restorecon / 降 permissive"
      err "另确认注入器是否为 aarch64 的 inj_a64，且已 su 提权"
    else
      err "so 未映射。注入器可能返回假句柄，换 aim-esp 版 injector_x64"
    fi
    return 1
  fi

  say "--- 关键日志 ---"
  logcat -d -s esp:V 2>/dev/null | grep -E "esp\.so|hook\(|GL_VERSION|ImGui|surface|\[shm\]|Perf" | tail -15

  if logcat -d -s esp:V 2>/dev/null | grep -q "ImGui"; then
    say "PASS  注入成功，ImGui 就绪，maps=${MAPS}"
  else
    say "WARN  已映射 ${MAPS} 段但暂无 ImGui 日志"
    [ "$PLAT" = "arm64" ] && say "      真机大厅 Camera 为 0，进对局后重跑 inject（或跑 verify 复查）"
  fi
  return 0
}

# ---------------------------------------------------------------- 换 JS
do_swap_js() {
  G="$GAME"
  [ -f "$DIR/index_v6.js" ] || [ -f "$DIR/index_v5.js" ] || { say "无新 JS（$DIR/index_v6.js），跳过换 JS"; return 0; }
  JS_NEW="$DIR/index_v6.js"
  [ -f "$JS_NEW" ] || JS_NEW="$DIR/index_v5.js"
  [ -d "$G" ] || { say "无 $G，跳过换 JS（先冷启动一次）"; return 0; }

  if [ -f "$G/index.js" ]; then
    cp -f "$G/index.js" "$G/index.js.bak_pre_v6" || die "备份 index.js 失败"
    say "已备份 -> $G/index.js.bak_pre_v6"
  fi
  OW=$(stat -c %u "$G" 2>/dev/null)
  GR=$(stat -c %g "$G" 2>/dev/null)
  cp -f "$JS_NEW" "$G/index.js" || die "写入 $G/index.js 失败"
  [ -n "$OW" ] && chown "$OW:$GR" "$G/index.js" 2>/dev/null
  chmod 600 "$G/index.js"
  restorecon "$G/index.js" 2>/dev/null
  chcon u:object_r:app_data_file:s0 "$G/index.js" 2>/dev/null
  md5sum "$G/index.js" "$JS_NEW" 2>/dev/null
  ls -lZ "$G/index.js" 2>/dev/null || ls -l "$G/index.js"
  return 0
}

# ---------------------------------------------------------------- 启动/停止
do_start() {
  fix_input_perm
  EXTRA="${1:-0}"
  case "$EXTRA" in ""|*[!0-9]*) EXTRA=0 ;; esac
  say "force-stop $PKG"
  am force-stop "$PKG" 2>/dev/null
  sleep 2
  ACTN=$(get_act)
  say "am start $ACTN"
  am start -n "$ACTN" 2>&1 | tail -2
  say "等待进程（最多 60s）"
  i=0
  PID=""
  while [ "$i" -lt 60 ]; do
    PID=$(getpid)
    [ -n "$PID" ] && break
    sleep 1
    i=$((i + 1))
  done
  [ -z "$PID" ] && die "60s 内没有进程"
  say "pid=$PID，等 ${BOOT_WAIT}s 让引擎起来"
  sleep "$BOOT_WAIT"
  [ "$EXTRA" -gt 0 ] 2>/dev/null && sleep "$EXTRA"
  return 0
}

do_stop() {
  P=$(getpid)
  if [ -z "$P" ]; then
    say "游戏未运行"
    return 0
  fi
  say "force-stop（pid=$P）"
  am force-stop "$PKG"
  sleep 1
  say "已停止，pid=$(getpid)"
  return 0
}

# ---------------------------------------------------------------- 体检
do_verify() {
  PID=$(getpid)
  say "pid = ${PID:-未运行}"
  [ -z "$PID" ] && return 1

  say "--- so 映射（应 ${EXPECT_MAPS} 段）---"
  grep "$SO_NAME" "/proc/$PID/maps" 2>/dev/null | head -5
  say "   段数: $(grep -c "$SO_NAME" "/proc/$PID/maps" 2>/dev/null)"

  say "--- esp 线程 ---"
  for t in /proc/$PID/task/*; do
    n=$(cat "$t/comm" 2>/dev/null)
    case "$n" in *esp*|*Esp*|*ensor*) say "   $n" ;; esac
  done
  say "   线程总数: $(ls /proc/$PID/task 2>/dev/null | wc -l)"

  say "--- 关键日志 ---"
  logcat -d -s esp:V 2>/dev/null | grep -E "hook\(|GL_VERSION|ImGui|surface|\[shm\]|Perf" | tail -12

  say "--- esp_data.bin（隔 2s 看是否在刷新）---"
  BIN=$(logcat -d -s esp:V 2>/dev/null | grep -o "/[^ ]*esp_data.bin" | tail -1)
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

# ---------------------------------------------------------------- 回滚
do_rollback() {
  say "force-stop"
  am force-stop "$PKG" 2>/dev/null
  sleep 2

  G="$GAME"
  if [ -f "$DIR/index_v3.js" ] && [ -d "$G" ]; then
    say "恢复 $DIR/index_v3.js -> index.js"
    [ -f "$G/index.js" ] && cp -f "$G/index.js" "$G/index.js.bak_before_rollback" 2>/dev/null
    cp -f "$DIR/index_v3.js" "$G/index.js" || die "写回 index.js 失败"
    OW=$(stat -c %u "$G" 2>/dev/null)
    GR=$(stat -c %g "$G" 2>/dev/null)
    [ -n "$OW" ] && chown "$OW:$GR" "$G/index.js" 2>/dev/null
    chmod 600 "$G/index.js"
    restorecon "$G/index.js" 2>/dev/null
    md5sum "$G/index.js" "$DIR/index_v3.js" 2>/dev/null
  else
    say "无基线 $DIR/index_v3.js 或缺 $G，只做 force-stop"
  fi

  if [ "$PLAT" = "arm64" ]; then
    LIBDIR=$(get_libdir 2>/dev/null)
    if [ -n "$LIBDIR" ] && [ -f "$LIBDIR/$SO_NAME" ]; then
      rm -f "$LIBDIR/$SO_NAME" && say "已卸载 $LIBDIR/$SO_NAME"
    fi
  fi
  say "回滚完成，pid=$(getpid)"
  return 0
}

# ---------------------------------------------------------------- 状态
report_file() {
  if [ -n "$1" ] && [ -f "$1" ]; then
    say "   $(ls -l "$1" 2>/dev/null | awk '{print $1, $5, $NF}')  md5=$(md5sum "$1" 2>/dev/null | cut -c1-32)"
  else
    say "   MISSING ${1:-<未找到适配本机 ABI 的文件>}"
  fi
}

do_status() {
  say "root=$(id -u)  pkg=$PKG  pid=$(getpid)"
  say "abi=${ABI:-?}  plat=${PLAT:-?}  selinux=$(getenforce 2>/dev/null)"
  say "--- 文件清单 ---"
  report_file "$SO_SRC"
  report_file "$INJ"
  report_file "$DIR/index_v6.js"
  report_file "$DIR/index_v3.js"
  say "--- $DIR 全部内容 ---"
  ls -l "$DIR" 2>/dev/null || say "   目录不存在: $DIR"
  if [ "$PLAT" = "arm64" ]; then
    LIBDIR=$(get_libdir 2>/dev/null)
    if [ -n "$LIBDIR" ]; then
      say "libdir=$LIBDIR  staged=$([ -f "$LIBDIR/$SO_NAME" ] && echo yes || echo no)"
    else
      say "libdir=获取失败（进程没起且 pm path 失败）"
    fi
  fi
  check_abi
  return 0
}

# ---------------------------------------------------------------- 日志/配置
do_log() {
  exec logcat -v time -s INJ:* ESP:* esp:V Cocos:* DEBUG:* | grep -Ei "esp|inject|$PKG"
}

do_cfg() {
  if [ -f "$CFG" ]; then
    cat "$CFG"
  else
    say "配置不存在: $CFG（启动游戏并调整一次悬浮窗后自动写入）"
  fi
}

usage() {
  cat <<EOF
esp.sh --- ESP 一体化脚本（x86_64 模拟器 / arm64 真机自动适配）

  all [auto|nojs]   换JS + 冷启动 + 注入 + 体检（默认动作）
  deploy            同 all
  stage             只把 so 铺进 App native lib 目录（arm64 必需）
  start [秒]        只冷启动，等指定秒数（默认 ${BOOT_WAIT}）
  stop              force-stop
  restart [秒]      stop + start + inject
  inject [pid]      只注入当前/指定进程
  verify            只读体检（maps/线程/日志/esp_data.bin）
  rollback          恢复基线 index.js + 卸载 staged so
  status            文件 + ABI + libdir + SELinux 全览
  log               实时过滤引擎日志
  cfg               查看悬浮窗配置 $CFG
  help              本帮助

环境变量: ESP_PKG ESP_ACT ESP_DIR ESP_BOOT_WAIT ESP_AUTO_WAIT
          ESP_STAGE_NAME ESP_GAME ESP_CFG ESP_EXPECT_MAPS
EOF
}

# ============================== main ==============================
case "$CMD" in
  help|-h|--help)
    usage
    exit 0
    ;;
  ""|all|deploy|stage|start|stop|restart|inject|verify|rollback|status|log|cfg)
    ;;
  *)
    echo "未知子命令: $CMD"
    usage
    exit 2
    ;;
esac

ensure_root
case "$CMD" in
  status) resolve_bins ;;
  *)      resolve_bins strict ;;
esac

case "$CMD" in
  ""|all|deploy)
    check_files
    check_abi
    if [ "$NOJS" = "1" ]; then say "nojs 模式，不换 JS"; else do_swap_js; fi
    do_start
    if [ "$AUTO" = "1" ]; then
      say "auto 模式：等 ${AUTO_WAIT}s 供手动进入对局/训练场，然后自动注入"
      sleep "$AUTO_WAIT"
    fi
    do_inject "$PID"
    echo
    do_verify
    if [ "$PLAT" = "arm64" ]; then
      say "提示：大厅 Camera 为 0，若上面没有 ImGui 日志，进对局后执行: sh $0 inject"
    fi
    ;;
  stage)
    do_stage
    ;;
  start)
    EXTRA="${2:-0}"
    do_start "$EXTRA"
    say "已启动 pid=$(getpid)；注入请执行: sh $0 inject"
    ;;
  stop)
    do_stop
    ;;
  restart)
    EXTRA="${2:-0}"
    do_stop
    sleep 2
    do_start "$EXTRA"
    do_inject "$PID"
    ;;
  inject)
    ARG="${2:-}"
    case "$ARG" in
      ''|*[!0-9]*) ARG="" ;;
    esac
    do_inject "$ARG"
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
  log)
    do_log
    ;;
  cfg)
    do_cfg
    ;;
esac
