// injector_arm64.c —— arm64-v8a 版 ptrace 注入器（真机 / 纯 arm64 环境）
//
// 与 injector_x64.c 的差异：
//   1. x86_64 的 struct user_regs_struct → arm64 的 struct user_pt_regs（34*8=272B）
//   2. SysV 调用约定(rdi/rsi/rdx/rcx/r8/r9) → AAPCS64(x0..x7)，返回值 x0
//   3. 返回哨兵在栈上(ret) → 返回地址在 x30(LR)，置 0 让函数返回时跳 0 触发 SIGSEGV
//   4. 栈对齐 rsp%16==8 → sp 必须 16 字节对齐（无栈上返回地址）
//   5. 远端调用栈空间从 0x1000 扩到 0x7000（arm64 dlopen 栈帧更深，避免爆栈）
//
// 用法: inj_a64 <pid> <远端 so 绝对路径>
//   root 必备: su -c "/data/local/tmp/imgui/inj_a64 <pid> /data/local/tmp/imgui/libesp_arm64.so"
//
// 编译: 见同目录 Android.mk / Application.mk
//   ndk-build.cmd NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk \
//                NDK_APPLICATION_MK=Application.mk NDK_TOOLCHAIN=clang -j4
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <dlfcn.h>

#if !defined(__aarch64__)
#error "injector_arm64.c 只能在 arm64 目标上编译（APP_ABI=arm64-v8a）"
#endif

#define LOG_TAG "inj64"
#define P(fmt, ...) do { printf(fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

// arm64 内核 ptrace 寄存器布局：34 个 8 字节 = 272 字节
// 直接对齐 bionic 的 struct user_pt_regs，避免依赖 <asm/ptrace.h> 在不同 NDK 版本的位置差异
struct arm64_pt_regs {
    unsigned long long regs[31];   // x0..x30
    unsigned long long sp;
    unsigned long long pc;
    unsigned long long pstate;
};

_Static_assert(sizeof(struct arm64_pt_regs) == 272, "arm64_pt_regs 必须为 272 字节");

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

#define MY_PROT_RWX   7
#define MAP_PRIVATE_ANON 0x22
#define RTLD_NOW_GLOBAL  0x102

// ------------------------------------------------------------------ 内存读写
// arm64 的 PTRACE_PEEKDATA 要求地址 8 字节对齐，逐字处理
static int pt_read(int pid, unsigned long addr, void *buf, size_t n) {
    size_t i = 0;
    while (i < n) {
        errno = 0;
        long v = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), 0);
        if (errno != 0) return -1;
        size_t c = (n - i >= sizeof(long)) ? sizeof(long) : (n - i);
        memcpy((char *)buf + i, &v, c);
        i += 8;                        // 地址推进量永远是 8（对齐要求）
    }
    return 0;
}

static int pt_write(int pid, unsigned long addr, const void *buf, size_t n) {
    size_t i = 0;
    while (i < n) {
        errno = 0;
        long old = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), 0);
        if (errno != 0) return -1;
        size_t c = (n - i >= sizeof(long)) ? sizeof(long) : (n - i);
        long nv = old;
        memcpy(&nv, (const char *)buf + i, c);
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(addr + i), (void *)nv) != 0) return -1;
        i += 8;
    }
    return 0;
}

// ------------------------------------------------------------------ 线程暂停
static int AttachAllThreads(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR *d = opendir(path);
    if (!d) { P("opendir task 失败: %s", strerror(errno)); return -1; }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        int tid = atoi(e->d_name);
        if (tid <= 0) continue;
        if (ptrace(PTRACE_ATTACH, tid, 0, 0) == 0) n++;
    }
    closedir(d);
    int status;
    for (int i = 0; i < n; ++i) waitpid(-1, &status, __WALL);
    P("已附加 %d 个线程", n);
    return n > 0 ? 0 : -1;
}

static int GetRegs(int tid, struct arm64_pt_regs *r) {
    struct iovec iov = { r, sizeof(*r) };
    if (ptrace(PTRACE_GETREGSET, tid, (void *)NT_PRSTATUS, &iov) != 0) return -1;
    return 0;
}

static int SetRegs(int tid, struct arm64_pt_regs *r) {
    struct iovec iov = { r, sizeof(*r) };
    if (ptrace(PTRACE_SETREGSET, tid, (void *)NT_PRSTATUS, &iov) != 0) return -1;
    return 0;
}

// ------------------------------------------------------------------ 远端调用
// AAPCS64: x0..x7 传参，返回值 x0；入口 sp 必须 16 字节对齐
static unsigned long RemoteCall(int pid, unsigned long fn,
                                unsigned long a1, unsigned long a2, unsigned long a3,
                                unsigned long a4, unsigned long a5, unsigned long a6,
                                unsigned long stackTop, int *sigOut) {
    struct arm64_pt_regs save, r;
    if (GetRegs(pid, &save) != 0) { P("GETREGSET(远调) 失败: %s", strerror(errno)); return 0; }
    r = save;
    if (stackTop == 0) stackTop = save.sp - 0x800;
    unsigned long sp = stackTop & ~0xFULL;      // 16 字节对齐，无栈上返回地址
    r.regs[0] = a1; r.regs[1] = a2; r.regs[2] = a3;
    r.regs[3] = a4; r.regs[4] = a5; r.regs[5] = a6;
    r.regs[30] = 0;                              // LR=0 → 函数返回 br x30 跳 0 → SIGSEGV
    r.sp = sp;
    r.pc = fn;
    if (SetRegs(pid, &r) != 0) { P("SETREGSET(远调) 失败: %s", strerror(errno)); return 0; }

    if (ptrace(PTRACE_CONT, pid, 0, 0) != 0) { P("CONT 失败"); return 0; }
    int status = 0;
    if (waitpid(pid, &status, __WALL) < 0) { P("waitpid 失败"); return 0; }
    int sig = WIFSTOPPED(status) ? WSTOPSIG(status) : -1;
    if (sigOut) *sigOut = sig;

    struct arm64_pt_regs after;
    unsigned long ret = 0;
    if (GetRegs(pid, &after) == 0) ret = (unsigned long)after.regs[0];
    P("远调 fn=%lx 信号=%d 返回=0x%lx (pc=%llx sp=%llx x0_old=%llx)",
      fn, sig, ret, after.pc, after.sp, save.regs[0]);

    if (SetRegs(pid, &save) != 0) P("恢复寄存器失败");
    if (sig == SIGSEGV || sig == SIGTRAP || sig == SIGILL) {
        siginfo_t si;
        if (ptrace(PTRACE_GETSIGINFO, pid, 0, &si) == 0)
            P("  信号详情: addr=%p code=%d", si.si_addr, si.si_code);
    }
    return ret;
}

// ------------------------------------------------------------------ 符号定位
static unsigned long RemoteModuleBase(int pid, const char *soname) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    unsigned long best = 0, anyMin = 0;
    size_t wantLen = strlen(soname);
    while (fgets(line, sizeof(line), f)) {
        char *hit = strstr(line, soname);
        if (!hit) continue;
        char nxt = hit[wantLen];
        if (nxt != '\n' && nxt != ' ' && nxt != '\0' && nxt != '\r') continue;
        unsigned long s = 0, e = 0, off = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s %lx", &s, &e, perms, &off) != 4) continue;
        if (anyMin == 0 || s < anyMin) anyMin = s;
        if (off == 0) { if (best == 0 || s < best) best = s; }   // 文件偏移 0 段 = 加载基址
    }
    fclose(f);
    if (!best) best = anyMin;
    return best;
}

static unsigned long FindRemoteSym(int pid, const char *soname, const char *sym) {
    void *h = dlopen(soname, RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen(soname, RTLD_NOW);
    if (!h) { P("本地 dlopen %s 失败: %s", soname, dlerror()); return 0; }
    void *p = dlsym(h, sym);
    if (!p) { P("本地 dlsym %s 失败: %s", sym, dlerror()); return 0; }
    Dl_info di;
    if (!dladdr(p, &di) || !di.dli_fbase) { P("dladdr 失败"); return 0; }
    unsigned long localOff = (unsigned long)p - (unsigned long)di.dli_fbase;
    // 必须用完整路径匹配：真机上 libc 落在 /apex/com.android.runtime/lib64/bionic/libc.so，
    // 且融合设备/模拟器上可能同时存在 houdini guest 的 32/64 位同名库，只按 basename 会命中错的。
    const char *full = di.dli_fname;
    unsigned long rbase = full ? RemoteModuleBase(pid, full) : 0;
    P("符号 %s: 本地模块=%s off=0x%lx 远端基址=0x%lx", sym, full ? full : "?", localOff, rbase);
    if (!rbase) return 0;
    return rbase + localOff;
}

// ------------------------------------------------------------------ 主流程
int main(int argc, char **argv) {
    if (argc < 3) { P("用法: %s <pid> <so绝对路径>", argv[0]); return 1; }
    int pid = atoi(argv[1]);
    const char *soPath = argv[2];
    if (pid <= 0) { P("pid 非法: %s", argv[1]); return 1; }
    if (access(soPath, R_OK) != 0) { P("远端路径不可读: %s", strerror(errno)); return 1; }
    P("目标进程 %d, 模块 %s", pid, soPath);
    if (geteuid() != 0) P("警告: 当前非 root(uid=%d)，ptrace 大概率被拒绝", geteuid());

    if (AttachAllThreads(pid) != 0) { P("附加失败"); return 1; }

    unsigned long pMmap   = FindRemoteSym(pid, "libc.so", "mmap");
    unsigned long pDlopen = FindRemoteSym(pid, "libdl.so", "dlopen");
    if (!pDlopen) pDlopen = FindRemoteSym(pid, "libc.so", "dlopen");
    unsigned long pDlerror = FindRemoteSym(pid, "libdl.so", "dlerror");
    if (!pDlerror) pDlerror = FindRemoteSym(pid, "libc.so", "dlerror");
    if (!pMmap || !pDlopen) {
        P("符号解析失败 mmap=%lx dlopen=%lx", pMmap, pDlopen);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 1;
    }

    int sig = 0;
    // 0x8000 缓冲：路径放 0x0，远端调用栈用 0x7000（留 0x7000 栈空间给 dlopen）
    unsigned long rmem = RemoteCall(pid, pMmap, 0, 0x8000, MY_PROT_RWX,
                                    MAP_PRIVATE_ANON, (unsigned long)-1, 0, 0, &sig);
    if (!rmem || (long)rmem == -1) {
        P("远端 mmap 失败: %ld sig=%d", (long)rmem, sig);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 1;
    }
    P("远端缓冲区 0x%lx", rmem);

    if (pt_write(pid, rmem, soPath, strlen(soPath) + 1) != 0) { P("写路径失败"); return 1; }
    char chk[256] = {0};
    pt_read(pid, rmem, chk, sizeof(chk));
    P("已写入路径: %s", chk);

    unsigned long h = RemoteCall(pid, pDlopen, rmem, RTLD_NOW_GLOBAL, 0, 0, 0, 0,
                                 rmem + 0x7000, &sig);
    P("dlopen 句柄 = 0x%lx", h);
    if (!h) {
        unsigned long e = RemoteCall(pid, pDlerror, 0, 0, 0, 0, 0, 0, rmem + 0x7000, &sig);
        if (e) { char msg[256] = {0}; pt_read(pid, e, msg, sizeof(msg) - 1); P("dlerror: %s", msg); }
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 1;
    }
    P("注入成功");

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            int tid = atoi(e->d_name);
            if (tid > 0) ptrace(PTRACE_DETACH, tid, 0, 0);
        }
        closedir(d);
    }
    ptrace(PTRACE_DETACH, pid, 0, 0);
    return 0;
}
