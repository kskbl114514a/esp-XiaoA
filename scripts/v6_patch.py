# -*- coding: utf-8 -*-
"""
v6 patch: base = index_v3.js (clean baseline, 已含锁血v2 + 子弹追踪 hooks)
输出 = index_v6.js

本次改动（全部为「面板直读倍率」语义，1:1 对应需求数值）：
  1) 移速  mul:3   —— 去掉旧公式里无依据的 3 常量，speedScale *= mul  => 3 倍
  2) 跳高  mul:4   —— 引擎原版高度基准 = 2.5（velocity 乘 sqrt(2.5)=1.5811），
                      改为 sqrt(2.5*mul) => 高度恰为原版 4 倍
  3) 射速  mul:2   （沿用，timeBetweenShots/2）
  4) 子弹追踪 __AIM_CFG__ 确保存在（bundle 内 shootBulletHelper/setShootAccuracy 钩子已就位）
  5) ESP_SHM v5 二进制写入块（JS 采集 -> ImGui 绘制，含 camp/aiLevel/liveState 透传）
步骤幂等；任何一项校验失败即中止，不产出半成品。
"""
import os, re, hashlib, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.join(HERE, "index_v3.js")
BLOCK = os.path.join(HERE, "esp_shm_block.js")
OUT = os.path.join(HERE, "index_v6.js")

MARK = "/*ESPSHM_V5_BEGIN*/"
ENDMARK = "/*ESPSHM_V5_END*/"
V3_ANCHOR = "return G.__V3CFG__={spd:5,rate:2,jump:4,fly:0};"

# ---- 移速：配置 + 公式
SPD_CFG_OLD = "G.__SPD_CFG__={on:!0,mul:5};"
SPD_CFG_NEW = "G.__SPD_CFG__={on:!0,mul:3};"
SPD_F_OLD = ("e.Movement.state.speedScale=e.Movement.state.speedScale*(3*"
             "((globalThis.__SPD_CFG__&&globalThis.__SPD_CFG__.on!==!1)"
             "?(globalThis.__SPD_CFG__.mul||1):1))+r.speed/100")
SPD_F_NEW = ("e.Movement.state.speedScale=e.Movement.state.speedScale*"
             "((globalThis.__SPD_CFG__&&globalThis.__SPD_CFG__.on!==!1)"
             "?(globalThis.__SPD_CFG__.mul||3):1)+r.speed/100")

# ---- 跳高：配置 + 公式（原版基准 2.5）
JMP_CFG_OLD = "G.__JUMP_CFG__={on:!0,heightMul:4};"
JMP_CFG_NEW = "G.__JUMP_CFG__={on:!0,mul:4};"
JMP_F_OLD = "var h=q.heightMul||2.5;return Math.sqrt(h)"
JMP_F_NEW = ("var b=2.5;var m=(q.mul!=null)?q.mul:((q.heightMul!=null)?q.heightMul/b:1);"
             "return Math.sqrt(b*m)")

# ---- 子弹追踪配置（若缺失则补默认；bundle 内已有钩子依赖此对象）
MJ_ANCHOR = "G.__MJ_CFG__={on:!1,segments:3,gapMs:200};"
AIM_DEF = ("G.__AIM_CFG__={on:!0,head:!1,maxDist:300,fov:70,alwaysHit:!0};"
           "   /* 子弹追踪/必中：on 追踪、head 打头、fov 半角(度)、alwaysHit 命中判定恒 1 */\n")


def die(msg):
    print("[FAIL] " + msg)
    sys.exit(1)


def sub1(s, old, new, tag):
    c = s.count(old)
    if c == 0:
        die("%s anchor not found" % tag)
    if c > 1:
        die("%s anchor ambiguous (x%d)" % (tag, c))
    print("[OK] %-14s %s -> %s" % (tag, old[-38:], new[-38:]))
    return s.replace(old, new, 1)


def main():
    s = open(BASE, encoding="utf-8", errors="replace", newline="").read()
    block = open(BLOCK, encoding="utf-8").read()
    n0 = len(s)

    # 0. 剥离旧 v5 cc.Graphics 自绘层（与 ImGui 重复绘制）
    if "/*ESP5_BEGIN*/" in s:
        s2 = re.sub(r"/\*ESP5_BEGIN\*/.*?/\*ESP5_END\*/", "", s, count=1, flags=re.S)
        if "/*ESP5_BEGIN*/" in s2 or "/*ESP5_END*/" in s2:
            die("ESP5 strip incomplete")
        print("[OK] stripped legacy ESP5 Graphics layer (%d -> %d)" % (len(s), len(s2)))
        s = s2
    else:
        print("[SKIP] no legacy ESP5 layer")

    # 1. 移速 3x
    s = sub1(s, SPD_CFG_OLD, SPD_CFG_NEW, "speed cfg")
    s = sub1(s, SPD_F_OLD, SPD_F_NEW, "speed formula")

    # 2. 跳高 4x
    s = sub1(s, JMP_CFG_OLD, JMP_CFG_NEW, "jump cfg")
    s = sub1(s, JMP_F_OLD, JMP_F_NEW, "jump formula")

    # 3. 子弹追踪配置（幂等：已存在则跳过）
    if "G.__AIM_CFG__={" in s:
        print("[SKIP] __AIM_CFG__ default already present")
    else:
        c = s.count(MJ_ANCHOR)
        if c != 1:
            die("MJ anchor count=%d (expect 1)" % c)
        s = s.replace(MJ_ANCHOR, MJ_ANCHOR + "\n" + AIM_DEF, 1)
        print("[OK] injected __AIM_CFG__ default (bullet track on / alwaysHit on)")

    # 4. ESP_SHM v5 写入块（插在 V3CFG return 之前 => 后定义生效，覆盖旧 txt writer）
    if MARK in s:
        print("[SKIP] espshm block already inserted")
    else:
        c = s.count(V3_ANCHOR)
        if c != 1:
            die("V3CFG anchor count=%d (expect 1)" % c)
        s = s.replace(V3_ANCHOR, MARK + "\n" + block + "\n" + ENDMARK + "\n" + V3_ANCHOR, 1)
        print("[OK] inserted ESP_SHM v5 writer block (%d bytes)" % len(block))

    # 5. 校验
    checks = {
        "speed mul:3": "G.__SPD_CFG__={on:!0,mul:3};",
        "speed f mul": "speedScale*((globalThis.__SPD_CFG__",
        "jump mul:4": "G.__JUMP_CFG__={on:!0,mul:4};",
        "jump f base2.5": "var b=2.5;var m=(q.mul!=null)",
        "rate mul:2": "G.__RATE_CFG__={on:!0,mul:2",
        "aim cfg": "G.__AIM_CFG__={on:!0,head:!1,maxDist:300,fov:70,alwaysHit:!0};",
        "aim hook": "shootBulletHelper(e,o,t,a){var m;",
        "aim alwaysHit": "alwaysHit!==!1",
        "esp cfg": "G.__ESP_CFG__=",
        "espshm export": "globalThis.__ESPSHM__=",
        "espshm bin": '"esp_data.bin"',
        "lockhp v2": "G.__LS_CFG__",
        "espshm mark": MARK,
        "espshm ver5": "var MAGIC=0x45535031,VER=5;",
        "espshm entsz360": "HDR_SZ=23152,ENT0=112,ENT_SZ=360",
        "espshm off_camp": "var O_CAMP=344,O_AI=348,O_LS=352;",
        "espshm w_camp": "dv.setInt32(base+O_CAMP",
        "espshm w_ai": "dv.setInt32(base+O_AI",
        "espshm w_live": "dv.setInt32(base+O_LS",
        "espshm rawcamp": "rawCamp:(d.camp!==void 0",
        "espshm passthru": "onlyReal:!1",
    }
    if "ESP5_BEGIN" in s or "__ESP5_COL__" in s:
        die("legacy ESP5 layer still present")
    if "G.__JUMP_CFG__={on:!0,heightMul:" in s:
        die("old jump cfg key still present")
    if "var h=q.heightMul||2.5;return Math.sqrt(h)" in s:
        die("old jump velocity formula still present")
    for k, v in checks.items():
        c = s.count(v)
        print("  %-16s x%d" % (k, c))
        if c < 1:
            die("verify failed: " + k)

    # 注入块括号配平
    b0, b1 = s.find(MARK), s.find(ENDMARK)
    seg = s[b0:b1]
    bal = {"{": 0, "(": 0, "[": 0}
    pairs = {"}": "{", ")": "(", "]": "["}
    for ch in seg:
        if ch in bal:
            bal[ch] += 1
        elif ch in pairs:
            bal[pairs[ch]] -= 1
    print("  block balance", bal)
    if any(v != 0 for v in bal.values()):
        die("block braces unbalanced")

    # 换行符保持原样写盘（禁止 CRLF 膨胀）
    with open(OUT, "w", encoding="utf-8", newline="") as f:
        f.write(s)
    raw = open(OUT, "rb").read()
    md5 = hashlib.md5(raw).hexdigest()
    crlf = raw.decode("utf-8", "replace").count("\r\n")
    print("[DONE] %s  %d -> %d bytes (crlf=%d)  md5=%s" % (OUT, n0, len(raw), crlf, md5))
    open(OUT + ".md5", "w", encoding="utf-8").write(md5 + "\n")


if __name__ == "__main__":
    main()
