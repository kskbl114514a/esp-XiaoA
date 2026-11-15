// hook.cpp —— inline hook 实现
#include "hook.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <unistd.h>
#include <sys/mman.h>
#include <link.h>

#if defined(__aarch64__)
#include "../third_party/And64InlineHook.hpp"
#endif

// ===================================================================== 工具
static size_t PageSize() { static size_t s = (size_t)sysconf(_SC_PAGESIZE); return s; }

static bool ProtectRWX(void* addr, size_t len) {
    uintptr_t ps = (uintptr_t)PageSize();
    uintptr_t start = (uintptr_t)addr & ~(ps - 1);
    uintptr_t end   = ((uintptr_t)addr + len + ps - 1) & ~(ps - 1);
    return mprotect((void*)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

static void FlushICache(void* addr, size_t len) {
#if defined(__aarch64__)
    __builtin___clear_cache((char*)addr, (char*)addr + len);
#else
    __builtin___clear_cache((char*)addr, (char*)addr + len);
#endif
}

// ===================================================================== maps
struct ModInfo { uintptr_t base; size_t size; };

static bool FindModule(const char* soname, ModInfo& out) {
    std::string want(soname);
    bool found = false;
    uintptr_t lo = ~0ULL, hi = 0;

    auto cb = [](struct dl_phdr_info* info, size_t, void* data) -> int {
        auto* d = (std::pair<std::string, std::pair<uintptr_t, uintptr_t>*>*)data;
        if (!info->dlpi_name || !info->dlpi_name[0]) return 0;
        std::string full(info->dlpi_name);
        size_t slash = full.find_last_of('/');
        std::string base = (slash == std::string::npos) ? full : full.substr(slash + 1);
        if (base != d->first) return 0;
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            const ElfW(Phdr)& ph = info->dlpi_phdr[i];
            if (ph.p_type == PT_LOAD) {
                uintptr_t s = info->dlpi_addr + ph.p_vaddr;
                uintptr_t e = s + ph.p_memsz;
                if (s < d->second->first)  d->second->first = s;
                if (e > d->second->second) d->second->second = e;
                d->second->first  = d->second->first;
            }
        }
        return 0;
    };

    std::pair<uintptr_t, uintptr_t> range(~0ULL, 0);
    std::pair<std::string, std::pair<uintptr_t, uintptr_t>*> arg{want, &range};
    dl_iterate_phdr(cb, &arg);
    if (range.first == ~0ULL || range.second == 0) return false;
    out.base = range.first;
    out.size = range.second - range.first;
    found = true;
    return found;
}

uintptr_t GetModuleBase(const char* soname) {
    ModInfo mi{0, 0};
    if (FindModule(soname, mi)) return mi.base;
    // 退化：直接扫 maps（某些库不在 phdr 里，例如动态加载后被 unmap 的 stub）
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    std::string want(soname);
    uintptr_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, want.c_str())) continue;
        uintptr_t s = strtoull(line, nullptr, 16);
        if (base == 0 || s < base) base = s;
    }
    fclose(f);
    return base;
}

size_t GetModuleSize(const char* soname) {
    ModInfo mi{0, 0};
    if (FindModule(soname, mi)) return mi.size;
    return 0;
}

bool IsExecutableAddr(uintptr_t addr) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s = 0, e = 0;
        char perm[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perm) != 3) continue;
        if (addr >= s && addr < e) { ok = (perm[2] == 'x'); break; }
    }
    fclose(f);
    return ok;
}

// ===================================================================== AOB
static bool ParsePattern(const char* pat, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask) {
    bytes.clear(); mask.clear();
    const char* p = pat;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (*p == '?') {
            bytes.push_back(0); mask.push_back(0);
            while (*p && *p != ' ') ++p;
        } else {
            char buf[3] = {p[0], p[1], 0};
            if (!isxdigit((unsigned char)buf[0]) || !isxdigit((unsigned char)buf[1])) return false;
            bytes.push_back((uint8_t)strtoul(buf, nullptr, 16));
            mask.push_back(1);
            p += 2;
        }
    }
    return !bytes.empty();
}

uintptr_t ScanPattern(const char* soname, const char* pattern, int maxHits, uintptr_t* outHits) {
    std::vector<uint8_t> pat, mask;
    if (!ParsePattern(pattern, pat, mask)) return 0;

    ModInfo mi{0, 0};
    if (!FindModule(soname, mi) || mi.size == 0) return 0;

    // 只在可执行段扫描
    struct Seg { uintptr_t start, end; };
    std::vector<Seg> segs;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s = 0, e = 0; char perm[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perm) != 3) continue;
        if (perm[2] != 'x') continue;
        if (s >= mi.base && s < mi.base + mi.size) segs.push_back({s, e});
    }
    fclose(f);

    int hits = 0;
    for (auto& sg : segs) {
        const uint8_t* p = (const uint8_t*)sg.start;
        const uint8_t* end = (const uint8_t*)sg.end;
        const size_t n = pat.size();
        for (const uint8_t* q = p; q + n <= end; ++q) {
            bool ok = true;
            for (size_t i = 0; i < n; ++i) {
                if (mask[i] && q[i] != pat[i]) { ok = false; break; }
            }
            if (ok) {
                if (outHits && hits < maxHits) outHits[hits] = (uintptr_t)q;
                if (++hits >= maxHits) return (uintptr_t)q;
                if (!outHits) return (uintptr_t)q;
            }
        }
    }
    return hits > 0 ? (uintptr_t)outHits[0] : 0;
}

#if defined(__aarch64__)
// ===================================================================== ARM64
struct ArmHookRec { void* target; uint8_t backup[32]; };
static std::map<void*, ArmHookRec*> g_armHooks;
static std::mutex g_armMtx;

bool InstallHook(void* target, void* hookFn, void** origFn) {
    if (!target || !hookFn) return false;
    std::lock_guard<std::mutex> lk(g_armMtx);
    auto* rec = new ArmHookRec();
    rec->target = target;
    memcpy(rec->backup, target, sizeof(rec->backup));

    void* tramp = nullptr;
    // And64InlineHook 内部会 mprotect + 写跳板，不需要我们准备页面
    A64HookFunction(target, hookFn, &tramp);
    if (!tramp) { LOGE("A64HookFunction 失败 target=%p", target); delete rec; return false; }
    if (origFn) *origFn = tramp;
    g_armHooks[target] = rec;
    LOGI("hook ok %p -> %p (tramp %p)", target, hookFn, tramp);
    return true;
}

bool UninstallHook(void* target) {
    std::lock_guard<std::mutex> lk(g_armMtx);
    auto it = g_armHooks.find(target);
    if (it == g_armHooks.end()) return false;
    if (!ProtectRWX(target, 32)) { LOGE("mprotect 失败"); return false; }
    memcpy(target, it->second->backup, sizeof(it->second->backup));
    FlushICache(target, 32);
    delete it->second;
    g_armHooks.erase(it);
    LOGI("unhook %p", target);
    return true;
}

#else
// ===================================================================== x86_64
// 极简指令长度解码器（覆盖库函数入口常见指令，用于确定至少 5 字节的完整指令边界）
namespace {
struct OpInfo { uint8_t hasModrm; uint8_t immSize; }; // immSize: 0/1/2/4/8；0xF0 表示随操作数大小

static const OpInfo kOne[256] = {
    /*00*/ {1,0},{1,0},{1,0},{1,0},{1,1},{1,4},{0,0},{0,0},
    /*08*/ {1,0},{1,0},{1,0},{1,0},{1,1},{1,4},{0,0},{0,0},
    /*10*/ {1,0},{1,0},{1,0},{1,0},{1,1},{1,4},{0,0},{0,0},
    /*18*/ {1,0},{1,0},{1,0},{1,0},{1,1},{1,4},{0,0},{0,0},
    /*20*/ {1,0},{1,0},{1,0},{1,0},{1,1},{1,4},{0,0},{0,0},
    /*28*/ {1,0},{1,0},{1,0},{1,0},{1,1},{1,4},{0,0},{0,0},
    /*30*/ {1,0},{1,0},{1,0},{1,0},{1,1},{1,4},{0,0},{0,0},
    /*38*/ {1,0},{1,0},{1,0},{1,0},{1,1},{1,4},{0,0},{0,0},
    /*40*/ {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},   // REX 前缀，单独处理
    /*48*/ {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
    /*50*/ {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},   // push/pop r64
    /*58*/ {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
    /*60*/ {0,0},{0,0},{0,0},{1,0},{0,0},{0,0},{0,0},{0,0},
    /*68*/ {0,4},{1,4},{0,1},{1,1},{0,0},{0,0},{0,0},{0,0},
    /*70*/ {0,1},{0,1},{0,1},{0,1},{0,1},{0,1},{0,1},{0,1},
    /*78*/ {0,1},{0,1},{0,1},{0,1},{0,1},{0,1},{0,1},{0,1},
    /*80*/ {1,1},{1,4},{1,0},{1,1},{1,0},{1,4},{1,0},{1,4},
    /*88*/ {1,0},{1,0},{1,0},{1,0},{1,0},{1,0},{1,0},{1,0},
    /*90*/ {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
    /*98*/ {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
    /*A0*/ {0,8},{0,8},{0,8},{0,8},{0,0},{0,0},{0,0},{0,0},
    /*A8*/ {0,1},{0,4},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
    /*B0*/ {0,1},{0,1},{0,1},{0,1},{0,1},{0,1},{0,1},{0,1},
    /*B8*/ {0,4},{0,4},{0,4},{0,4},{0,4},{0,4},{0,4},{0,4},
    /*C0*/ {1,1},{1,1},{0,2},{0,0},{0,0},{0,0},{1,1},{0,4},
    /*C8*/ {0,4},{0,0},{0,2},{0,0},{0,0},{0,0},{1,0},{0,0},
    /*D0*/ {1,0},{1,0},{1,0},{1,0},{0,0},{0,0},{0,0},{0,0},
    /*D8*/ {1,0},{1,0},{1,0},{1,0},{1,0},{1,0},{1,0},{1,0},
    /*E0*/ {0,1},{0,1},{0,1},{0,1},{0,1},{0,1},{0,1},{0,1},
    /*E8*/ {0,4},{0,4},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
    /*F0*/ {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{1,0},{1,0},
    /*F8*/ {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{1,0},{1,0},
};

static int ModrmLen(const uint8_t* p, int rexW) {
    uint8_t modrm = p[0];
    int mod = modrm >> 6, rm = modrm & 7;
    int len = 1;
    if (mod != 3) {
        if (rm == 4) {                 // SIB
            uint8_t sib = p[1];
            len++;
            if (mod == 0 && (sib & 7) == 5) len += 4;
        }
        if (mod == 0 && rm == 5) len += 4;        // RIP-relative
        else if (mod == 1) len += 1;
        else if (mod == 2) len += 4;
    } else if (rexW) { /* nothing */ }
    return len;
}

// 返回指令长度；失败返回 0
static int InstLength(const uint8_t* code, int maxLen) {
    int i = 0;
    bool opsz16 = false, rexW = false;
    while (i < maxLen) {
        uint8_t b = code[i];
        if (b == 0x66) { opsz16 = true; i++; continue; }
        if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) { i++; continue; }
        if (b >= 0x40 && b <= 0x4F) { rexW = (b & 8) != 0; i++; continue; }
        break;
    }
    if (i >= maxLen) return 0;

    uint8_t op = code[i++];
    const OpInfo& oi = kOne[op];
    if (op == 0x0F) {                       // 两字节 opcode
        if (i >= maxLen) return 0;
        uint8_t op2 = code[i++];
        // 0F 3A / 0F 38 三字节
        if (op2 == 0x38 || op2 == 0x3A) {
            if (i >= maxLen) return 0;
            i++;
            if (i >= maxLen) return 0;
            i += ModrmLen(code + i, rexW);
            return i;
        }
        // 0F 80-8F jcc rel32
        if (op2 >= 0x80 && op2 <= 0x8F) return i + 4;
        // 0F 70-73 有 modrm + imm8
        if (op2 >= 0x70 && op2 <= 0x73) return i + ModrmLen(code + i, rexW) + 1;
        // 0F 1F / 0F 18 / 0F 10..17 等：带 modrm
        static const char* withModrm = "\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1A\x1B\x1C\x1D\x1E\x1F"
                                       "\x28\x29\x2A\x2B\x2C\x2D\x2E\x2F\x40\x41\x42\x43\x44\x45\x46\x47"
                                       "\x48\x49\x4A\x4B\x4C\x4D\x4E\x4F\x50\x51\x52\x53\x54\x55\x56\x57"
                                       "\x58\x59\x5A\x5B\x5C\x5D\x5E\x5F\x60\x61\x62\x63\x64\x65\x66\x67"
                                       "\x68\x69\x6A\x6B\x6C\x6D\x6E\x6F\x70\x71\x72\x73\x74\x75\x76\x77"
                                       "\x78\x79\x7A\x7B\x7C\x7D\x7E\x7F\x90\x91\x92\x93\x94\x95\x96\x97"
                                       "\x98\x99\x9A\x9B\x9C\x9D\x9E\x9F\xA3\xA4\xA5\xAB\xAC\xAD\xAE\xAF"
                                       "\xB0\xB1\xB2\xB3\xB4\xB5\xB6\xB7\xB8\xB9\xBA\xBB\xBC\xBD\xBE\xBF"
                                       "\xC0\xC1\xC2\xC3\xC4\xC5\xC6\xC7\xC8\xC9\xCA\xCB\xCC\xCD\xCE\xCF";
        for (int k = 0; withModrm[k]; ++k) {
            if ((uint8_t)withModrm[k] == op2) return i + ModrmLen(code + i, rexW);
        }
        return 0;   // 未知两字节指令，放弃
    }
    if (!oi.hasModrm) return i + oi.immSize;
    int ml = 0;
    if (i < maxLen) ml = ModrmLen(code + i, rexW);
    i += ml;
    int imm = oi.immSize;
    if (op == 0xC7 && imm == 4) imm = opsz16 ? 2 : 4;
    if ((op == 0x81) && imm == 4) imm = opsz16 ? 2 : 4;
    // F6/F7 group3: /0 /1 带 imm
    if (op == 0xF6 || op == 0xF7) {
        uint8_t modrm = code[i - ml];
        uint8_t reg = (modrm >> 3) & 7;
        if (reg <= 1) { i += (op == 0xF6) ? 1 : (opsz16 ? 2 : 4); }
        return i;
    }
    if (op == 0xFE || op == 0xFF || op == 0x8D || op == 0x8B) imm = 0;
    // push/pop/call/jmp r/m64 (FF /6,/4,/2) 无立即数
    return i + imm;
}

struct XHookRec {
    void* target; int patchLen; uint8_t backup[32]; void* tramp; size_t trampSize;
};
}  // namespace

static std::map<void*, XHookRec*> g_xHooks;
static std::mutex g_xMtx;

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

// 在 target ±2GB 内找一个空闲页，保证 rel32 能跳过去。
// 直接 mmap 任意地址会落到很远的地方（Android 上常见 >2GB），rel32 不可达，
// 而 14 字节 abs64 jmp 又常常吃不下函数序言 —— 所以必须分配"近页"。
static void* AllocNearExec(uintptr_t target) {
    size_t ps = PageSize();
    const size_t step = 0x10000;
    for (size_t delta = step; delta < 0x70000000; delta += step) {
        for (int dir = 0; dir < 2; ++dir) {
            uintptr_t addr = dir ? (target + delta) : (target - delta);
            addr &= ~(uintptr_t)(ps - 1);
            if (!addr) continue;
            void* p = mmap((void*)addr, ps, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p == MAP_FAILED) continue;
            intptr_t d = (intptr_t)p - (intptr_t)target;
            if (d > INT32_MIN && d < INT32_MAX) return p;
            munmap(p, ps);
        }
    }
    void* p = mmap(nullptr, ps, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

bool InstallHook(void* target, void* hookFn, void** origFn) {
    if (!target || !hookFn) return false;
    std::lock_guard<std::mutex> lk(g_xMtx);

    const uint8_t* t = (const uint8_t*)target;
    int off = 0;
    while (off < 5) {
        int l = InstLength(t + off, 16);
        if (l <= 0) { LOGE("x86 解码失败 target=%p off=%d", target, off); return false; }
        off += l;
    }
    int len = off;
    if (len > 24) { LOGE("x86 覆盖过长 %d", len); return false; }

    auto* rec = new XHookRec();
    rec->target = target;
    rec->patchLen = len;
    memcpy(rec->backup, target, len);

    size_t ps = PageSize();
    void* tramp = AllocNearExec((uintptr_t)target);
    if (!tramp) { delete rec; return false; }
    uint8_t* tp = (uint8_t*)tramp;
    memset(tp, 0xCC, ps);

    // [0,len)           = 目标处原指令
    // [len,len+14)      = abs64 jmp 回 target+len（orig 调用返回用）
    // [len+14,len+28)   = abs64 jmp 到 hookFn（近跳板用）
    memcpy(tp, target, len);
    uint8_t* back = tp + len;
    back[0] = 0xFF; back[1] = 0x25; back[2] = back[3] = back[4] = back[5] = 0x00;
    uint64_t backAddr = (uint64_t)((uintptr_t)target + len);
    memcpy(back + 6, &backAddr, 8);

    uint8_t* thunk = tp + len + 16;
    thunk[0] = 0xFF; thunk[1] = 0x25; thunk[2] = thunk[3] = thunk[4] = thunk[5] = 0x00;
    uint64_t dst = (uint64_t)(uintptr_t)hookFn;
    memcpy(thunk + 6, &dst, 8);

    rec->tramp = tramp;
    rec->trampSize = ps;
    FlushICache(tramp, len + 32);

    if (!ProtectRWX(target, len)) { munmap(tramp, ps); delete rec; return false; }

    std::vector<uint8_t> patch(len, 0x90);
    // 目标处统一写 5 字节 rel32；远端不可达时跳到近跳板，由它做 abs64 中转
    void* jmpDst = hookFn;
    intptr_t diff = (intptr_t)hookFn - ((intptr_t)target + 5);
    if (diff <= INT32_MIN || diff >= INT32_MAX) {
        jmpDst = thunk;
        diff = (intptr_t)thunk - ((intptr_t)target + 5);
        if (diff <= INT32_MIN || diff >= INT32_MAX) {
            LOGE("x86 近跳板仍不可达 target=%p thunk=%p", target, thunk);
            munmap(tramp, ps); delete rec; return false;
        }
    }
    patch[0] = 0xE9;
    int32_t d32 = (int32_t)diff;
    memcpy(&patch[1], &d32, 4);
    memcpy(target, patch.data(), len);
    FlushICache(target, len);

    if (origFn) *origFn = tramp;
    g_xHooks[target] = rec;
    LOGI("hook(x64) ok %p -> %p len=%d tramp=%p thunk=%p", target, hookFn, len, tramp, thunk);
    return true;
}

bool UninstallHook(void* target) {
    std::lock_guard<std::mutex> lk(g_xMtx);
    auto it = g_xHooks.find(target);
    if (it == g_xHooks.end()) return false;
    if (!ProtectRWX(target, it->second->patchLen)) return false;
    memcpy(target, it->second->backup, it->second->patchLen);
    FlushICache(target, it->second->patchLen);
    if (it->second->tramp) munmap(it->second->tramp, it->second->trampSize);
    delete it->second;
    g_xHooks.erase(it);
    return true;
}
#endif
