// injector.cpp —— arm64 ptrace 远程 dlopen 注入器（无 magisk / 纯 root 场景）
//
// 用法（在目标设备上以 root 运行）：
//   injector64 <pid> <so 绝对路径>
//   例：injector64 4168 /data/local/tmp/libesp.so
//
// 原理：
//   1) PTRACE_ATTACH 挂起目标线程，PTRACE_GETREGSET(NT_PRSTATUS) 拿到 user_pt_regs
//   2) 用目标进程自己 libc 里的 mmap 开一页 RWX 暂存区，把 so 路径写进去
//   3) 把 sp 切到该页，pc 指向目标进程内 dlopen 的真实地址，lr = 0 作为返回哨兵
//   4) PTRACE_CONT → 函数返回时跳地址 0 触发 SIGSEGV，即"调用结束"信号
//   5) 读回 x0 得到句柄，恢复寄存器，PTRACE_DETACH
//
// 注：so 内 __attribute__((constructor)) 会在 dlopen 返回前执行，无需额外调入口。

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <link.h>
#include <elf.h>

#if !defined(__aarch64__)
#error "injector 只支持 arm64（目标进程为 arm64/houdini 转译环境）"
#endif

#define NT_PRSTATUS 1

struct user_pt_regs64 {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

static bool ReadRegs(pid_t tid, user_pt_regs64& r) {
    iovec io{&r, sizeof(r)};
    return ptrace(PTRACE_GETREGSET, tid, (void*)NT_PRSTATUS, &io) >= 0;
}
static bool WriteRegs(pid_t tid, const user_pt_regs64& r) {
    iovec io{(void*)&r, sizeof(r)};
    return ptrace(PTRACE_SETREGSET, tid, (void*)NT_PRSTATUS, &io) >= 0;
}

// ---------------------------------------------------------------- maps 解析
struct MapEntry { uint64_t start, end, offset; std::string path; };

static std::vector<MapEntry> ReadMaps(pid_t pid) {
    std::vector<MapEntry> out;
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/maps", pid);
    FILE* f = fopen(p, "r");
    if (!f) return out;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long s = 0, e = 0, off = 0;
        char perm[8] = {0}, path[768] = {0};
        int n = sscanf(line, "%llx-%llx %7s %llx %*s %*s %767s", &s, &e, perm, &off, path);
        if (n < 5 || !strchr(perm, 'x')) continue;      // 只关心可执行段（用于定位模块基址）
        MapEntry m; m.start = s; m.end = e; m.offset = off; m.path = path;
        out.push_back(m);
    }
    fclose(f);
    return out;
}

// 模块在目标进程中的加载基址 = 第一个该模块映射的 start - offset
static uint64_t ModuleBase(pid_t pid, const char* needle) {
    for (auto& m : ReadMaps(pid)) {
        if (m.path.find(needle) != std::string::npos) return m.start - m.offset;
    }
    return 0;
}

// ---------------------------------------------------------------- 本地符号偏移
// 在注入器自身进程里解析出 fn 所属模块的加载基址，得到模块内偏移
static uint64_t LocalOffset(void* fn, std::string* modName) {
    Dl_info info{};
    if (!dladdr(fn, &info) || !info.dli_fbase) return 0;
    if (modName) {
        *modName = info.dli_fname ? info.dli_fname : "";
        // 只保留文件名，maps 里是完整路径，用 basename 也能命中
    }
    return (uint64_t)fn - (uint64_t)info.dli_fbase;
}

// ---------------------------------------------------------------- 远程读写
static bool RemoteWrite(pid_t tid, uint64_t dst, const void* src, size_t len) {
    const uint8_t* p = (const uint8_t*)src;
    for (size_t i = 0; i < len; i += sizeof(long)) {
        long word = 0;
        size_t n = (len - i < sizeof(long)) ? (len - i) : sizeof(long);
        memcpy(&word, p + i, n);
        if (ptrace(PTRACE_POKEDATA, tid, (void*)(dst + i), (void*)word) < 0) return false;
    }
    return true;
}
static bool RemoteRead(pid_t tid, uint64_t src, void* dst, size_t len) {
    uint8_t* p = (uint8_t*)dst;
    for (size_t i = 0; i < len; i += sizeof(long)) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, tid, (void*)(src + i), nullptr);
        if (word == -1 && errno) return false;
        size_t n = (len - i < sizeof(long)) ? (len - i) : sizeof(long);
        memcpy(p + i, &word, n);
    }
    return true;
}

// ---------------------------------------------------------------- 远程调用
// 返回 true 表示调用完成（未必成功），ret 为 x0
static bool RemoteCall(pid_t tid, uint64_t fn, uint64_t sp, uint64_t lrSentinel,
                       const uint64_t* args, int nargs, uint64_t* ret) {
    user_pt_regs64 saved{}, r{};
    if (!ReadRegs(tid, saved)) { fprintf(stderr, "GETREGSET 失败: %s\n", strerror(errno)); return false; }
    r = saved;
    for (int i = 0; i < nargs && i < 8; ++i) r.regs[i] = args[i];
    r.pc = fn;
    r.sp = sp;
    r.regs[30] = lrSentinel;          // x30 = 返回地址 = 哨兵（0）
    if (!WriteRegs(tid, r)) { fprintf(stderr, "SETREGSET 失败: %s\n", strerror(errno)); return false; }

    bool ok = false;
    for (int guard = 0; guard < 64; ++guard) {
        if (ptrace(PTRACE_CONT, tid, nullptr, nullptr) < 0) break;
        int st = 0;
        if (waitpid(tid, &st, __WALL) < 0) break;
        if (WIFEXITED(st)) break;
        if (WIFSTOPPED(st)) {
            int sig = WSTOPSIG(st);
            if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGTRAP) {
                user_pt_regs64 after{};
                if (ReadRegs(tid, after)) {
                    *ret = after.regs[0];
                    // 正常返回会停在 pc≈0（哨兵）；若 pc 仍指向函数体内，说明内部崩了
                    ok = (after.pc < 4096);
                }
                break;
            }
            // 其它信号（SIGSTOP 之类）丢掉继续跑
            continue;
        }
    }
    // 无论成败，恢复现场
    WriteRegs(tid, saved);
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <pid> <so绝对路径>\n", argv[0]);
        return 1;
    }
    pid_t pid = (pid_t)atoi(argv[1]);
    std::string so = argv[2];
    if (pid <= 0 || so.empty() || so[0] != '/' || access(so.c_str(), R_OK) != 0) {
        fprintf(stderr, "参数无效或 so 不可读: %s\n", so.c_str());
        return 1;
    }

    // 本地解析符号：dlopen / mmap
    void* hLibc = dlopen("libc.so", RTLD_NOW);
    if (!hLibc) { fprintf(stderr, "本地 dlopen libc 失败: %s\n", dlerror()); return 1; }
    void* local_dlopen = dlsym(hLibc, "dlopen");
    void* local_mmap   = dlsym(hLibc, "mmap");
    if (!local_dlopen || !local_mmap) { fprintf(stderr, "拿不到 dlopen/mmap\n"); return 1; }

    std::string modName;
    uint64_t offDlopen = LocalOffset(local_dlopen, &modName);
    uint64_t offMmap   = LocalOffset(local_mmap, nullptr);
    const char* baseName = strrchr(modName.c_str(), '/');
    baseName = baseName ? baseName + 1 : modName.c_str();
    fprintf(stderr, "[*] 本地符号: %s dlopen=+0x%llx mmap=+0x%llx\n",
            baseName, (unsigned long long)offDlopen, (unsigned long long)offMmap);

    uint64_t base = ModuleBase(pid, baseName);
    if (!base) {
        // 有些 ROM 里 dlopen 在 libdl.so
        base = ModuleBase(pid, "libdl.so");
        if (!base) { fprintf(stderr, "[-] 目标进程里找不到 %s / libdl.so\n", baseName); return 1; }
    }
    uint64_t fnDlopen = base + offDlopen;
    uint64_t fnMmap   = base + offMmap;
    fprintf(stderr, "[*] 目标 %d 模块基址=0x%llx dlopen=0x%llx mmap=0x%llx\n",
            pid, (unsigned long long)base, (unsigned long long)fnDlopen, (unsigned long long)fnMmap);

    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) < 0) {
        fprintf(stderr, "[-] PTRACE_ATTACH 失败: %s（需要 root，且不能被反调试拦截）\n", strerror(errno));
        return 1;
    }
    int st = 0;
    waitpid(pid, &st, __WALL);

    user_pt_regs64 saved{};
    if (!ReadRegs(pid, saved)) { fprintf(stderr, "[-] 读寄存器失败\n"); ptrace(PTRACE_DETACH, pid, 0, 0); return 1; }

    // 1) 远程 mmap 一块 RWX（x0=addr x1=len x2=prot x3=flags x4=fd x5=off）
    uint64_t a[6] = {0, 0x8000, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0};
    uint64_t rbuf = 0;
    if (!RemoteCall(pid, fnMmap, saved.sp, 0, a, 6, &rbuf) || rbuf == 0 || rbuf > 0x0000ffffffffffffULL) {
        fprintf(stderr, "[-] 远程 mmap 失败 (ret=0x%llx)\n", (unsigned long long)rbuf);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 1;
    }
    fprintf(stderr, "[*] 远程暂存区: 0x%llx\n", (unsigned long long)rbuf);

    // 2) 写入 so 路径（放在暂存区头部）
    if (!RemoteWrite(pid, rbuf, so.c_str(), so.size() + 1)) {
        fprintf(stderr, "[-] 写入路径失败\n");
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 1;
    }

    // 3) 远程 dlopen(path, RTLD_NOW)
    uint64_t sp = rbuf + 0x7000;   // 栈顶放在暂存区尾部，向下生长
    uint64_t args[2] = {rbuf, RTLD_NOW};
    uint64_t h = 0;
    bool ok = RemoteCall(pid, fnDlopen, sp, 0, args, 2, &h);
    fprintf(stderr, "%s dlopen -> 0x%llx\n", ok ? "[+]" : "[!]", (unsigned long long)h);
    if (!ok || h == 0) {
        fprintf(stderr, "[-] 注入失败，检查：架构是否一致(arm64)、so 依赖是否齐全、SELinux 是否拦截\n");
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 2;
    }

    // 4) 调 dlsym 拿 constructor 不需要，直接看日志即可；这里顺手清掉暂存区代码权限问题（保持映射，进程退出自清）
    ptrace(PTRACE_DETACH, pid, 0, 0);
    fprintf(stderr, "[+] 注入完成，现在看 logcat：adb logcat -s ESP:*\n");
    return 0;
}
