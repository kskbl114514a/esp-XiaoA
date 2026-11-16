# ==========================================================================
#  deploy_arm64.ps1 --- Windows 侧一键部署到真机（arm64 + root/KernelSU）
#  用法：
#    powershell -ExecutionPolicy Bypass -File scripts\deploy_arm64.ps1
#    powershell -ExecutionPolicy Bypass -File scripts\deploy_arm64.ps1 -Run status
#
#  做的事：校验本地产物架构/md5 -> adb push 四件套 -> chmod -> 可选执行设备端子命令
# ==========================================================================
param(
  [string]$Serial = "",
  [string]$Run = "",                       # status / stage / inject / verify / all / rollback
  [string]$Adb = "C:\Users\ROG\AppData\Local\Android\Sdk\platform-tools\adb.exe"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$D    = "/data/local/tmp/imgui"

if (-not (Test-Path $Adb)) { $Adb = (Get-Command adb -ErrorAction SilentlyContinue).Source }
if (-not $Adb) { throw "找不到 adb，请改 -Adb 参数" }

function A { param([string[]]$a) if ($Serial) { & $Adb -s $Serial @a } else { & $Adb @a } }

# ---- 0. 设备 ----
$devs = (& $Adb devices | Select-Object -Skip 1 | Where-Object { $_ -match "\tdevice$" })
if (-not $devs) { throw "没有已授权的设备，先插线并确认 adb 授权弹窗" }
if (-not $Serial -and $devs.Count -gt 1) { throw "多台设备，请用 -Serial 指定" }
$abi = (A @("shell","getprop","ro.product.cpu.abi")).Trim()
$enforce = (A @("shell","getenforce")).Trim()
$rootId = (A @("shell","su","-c","id -u")).Trim()
Write-Host "[deploy] device abi=$abi  selinux=$enforce  su-id=$rootId" -ForegroundColor Cyan
if ($abi -notmatch "arm64") { throw "设备 abi=$abi，不是 arm64；模拟器请走 esp_v5.sh 流程" }
if ($rootId -ne "0") { throw "su 未拿到 root（返回 '$rootId'），检查 KernelSU 是否已授权该 shell" }

# ---- 1. 本地产物校验 ----
$files = @(
  @{ host = Join-Path $repo "prebuilt\arm64\inj_a64";               dev = "$D/inj_a64";        mode = "755"; mach = "b700"; md5 = "c109c3460b2fa62750035d7994b92d0a" },
  @{ host = Join-Path $repo "prebuilt\libesp_arm64-v8a.so";         dev = "$D/libesp_arm64.so";mode = "644"; mach = "b700"; md5 = "49ecf9939e375c90bb970ac479d8a017" },
  @{ host = Join-Path $repo "scripts\esp_a64.sh";                   dev = "$D/esp_a64.sh";     mode = "755"; mach = "";     md5 = "" },
  @{ host = Join-Path $repo "js\esp_shm_block.js";                  dev = "$D/index_v6.js";    mode = "644"; mach = "";     md5 = "" }
)
foreach ($f in $files) {
  if (-not (Test-Path $f.host)) { throw "缺文件 $($f.host)" }
  $b = [System.IO.File]::ReadAllBytes($f.host)
  $hash = (Get-FileHash $f.host -Algorithm MD5).Hash.ToLower()
  if ($f.md5 -and $hash -ne $f.md5) { throw "$($f.host) md5=$hash，期望 $($f.md5)（产物被改过，先重新编译）" }
  if ($f.mach) {
    $m = "{0:x2}{1:x2}" -f $b[18], $b[19]
    if ($m -ne $f.mach) { throw "$($f.host) e_machine=$m，期望 $($f.mach)（不是 aarch64）" }
  }
  Write-Host ("[deploy] ok  {0,-34} {1,9} B  md5={2}" -f (Split-Path $f.host -Leaf), $b.Length, $hash)
}

# ---- 2. push ----
A @("shell","su","-c","mkdir -p $D") | Out-Null
foreach ($f in $files) {
  Write-Host "[deploy] push -> $($f.dev)"
  A @("push", $f.host, $f.dev) | Select-Object -Last 1 | Out-Null
}
$cmds = ($files | ForEach-Object { "chmod $($_.mode) $($_.dev)" }) -join "; "
A @("shell","su","-c",$cmds) | Out-Null

# ---- 3. 设备端复核 ----
Write-Host "[deploy] 设备端清单:" -ForegroundColor Cyan
$ls = A @("shell","su","-c","ls -l $D; echo ---; md5sum $D/inj_a64 $D/libesp_arm64.so")
$ls | ForEach-Object { "  $_" }

# ---- 4. 可选执行 ----
if ($Run) {
  if ($Run -eq "all") { $dev = "all nojs" } else { $dev = $Run }
  Write-Host "[deploy] 执行: sh $D/esp_a64.sh $dev" -ForegroundColor Yellow
  A @("shell","su","-c","sh $D/esp_a64.sh $dev") | ForEach-Object { "  $_" }
} else {
  Write-Host ""
  Write-Host "下一步（按顺序）:" -ForegroundColor Green
  Write-Host "  1) su -c `"sh $D/esp_a64.sh all nojs`"      # 换JS(可选)+冷启动游戏"
  Write-Host "  2) 手动进训练场/对局（大厅里 Camera=0 注不进去层）"
  Write-Host "  3) su -c `"sh $D/esp_a64.sh inject`"        # 注入，验收 maps=3"
  Write-Host "  4) su -c `"sh $D/esp_a64.sh verify`"        # 体检"
  Write-Host "  或直接 -Run all 走 auto（等同 all，需手动进对局后再 inject）"
}
