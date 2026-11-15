// injector_x64.c —— x86_64 版 ptrace 注入器
// 适配 x86_64 + houdini 混血环境：arm64 二进制无法 GETREGSET(NT_PRSTATUS)（EINVAL），
// 只有 x86_64 注入器能读寄存器并完成远端调用，因此被注入模块也必须是 x86_64 ELF。
//
// 用法: injector_x64 <pid> <远端 so 绝对路径>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <dlfcn.h>

#define LOG_TAG "inj64"
#define P(fmt, ...) do { printf(fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

// 常量（避免依赖 bionic 私有头）
#define MY_PROT_RWX   7
#define MAP_PRIVATE_ANON 0x22
#define RTLD_NOW_GLOBAL  0x102

static int g_pid = 0;

// ------------------------------------------------------------------ 内存读写
static int pt_read(int pid, unsigned long addr, void *buf, size_t n) {
    size_t i = 0;
    while (i < n) {
        errno = 0;
        long v = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + i), 0);
        if (errno != 0) return -1;
        size_t c = (n - i >= sizeof(long)) ? sizeof(long) : (n - i);
        memcpy((char *)buf + i, &v, c);
        i += c;
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
        i += c;
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
        if (ptrace(PTRACE_ATTACH, tid, 0, 0) == 0) { n++; }
        else { /* 可能是僵尸线程，忽略 */ }
    }
    closedir(d);
    // 回收所有 stop 事件
    int status;
    for (int i = 0; i < n; ++i) waitpid(-1, &status, __WALL);
    P("已附加 %d 个线程", n);
    return n > 0 ? 0 : -1;
}

static int GetRegs(int tid, struct user_regs_struct *r) {
    struct iovec iov = { r, sizeof(*r) };
    if (ptrace(PTRACE_GETREGSET, tid, (void *)1 /*NT_PRSTATUS*/, &iov) != 0) return -1;
    return 0;
}

static int SetRegs(int tid, struct user_regs_struct *r) {
    struct iovec iov = { r, sizeof(*r) };
    if (ptrace(PTRACE_SETREGSET, tid, (void *)1, &iov) != 0) return -1;
    return 0;
}

// ------------------------------------------------------------------ 远端调用
// x86_64 SysV: rdi rsi rdx rcx r8 r9；返回值 rax
static unsigned long RemoteCall(int pid, unsigned long fn,
                                unsigned long a1, unsigned long a2, unsigned long a3,
                                unsigned long a4, unsigned long a5, unsigned long a6,
                                unsigned long stackTop, int *sigOut) {
    struct user_regs_struct save, r;
    if (GetRegs(pid, &save) != 0) { P("GETREGS(远调) 失败: %s", strerror(errno)); return 0; }
    r = save;
    // stackTop==0 时借用被附加线程自己的栈空间（已映射，且我们附加期间该线程不会跑）
    if (stackTop == 0) stackTop = save.rsp - 0x400;
    // 栈：SysV 进入函数时 rsp%16==8，先放 0 作为返回地址哨兵
    unsigned long sp = (stackTop & ~0xFUL) - 8;   // 留 8 字节给 ret 哨兵，且满足 rsp%16==8
    unsigned long retAddr = 0;
    if (pt_write(pid, sp, &retAddr, sizeof(retAddr)) != 0) { P("写返回哨兵失败"); return 0; }
    r.rsp = sp;
    r.rip = fn;
    r.rdi = a1; r.rsi = a2; r.rdx = a3; r.rcx = a4; r.r8 = a5; r.r9 = a6;
    if (SetRegs(pid, &r) != 0) { P("SETREGS(远调) 失败: %s", strerror(errno)); return 0; }

    if (ptrace(PTRACE_CONT, pid, 0, 0) != 0) { P("CONT 失败"); return 0; }
    int status = 0;
    if (waitpid(pid, &status, __WALL) < 0) { P("waitpid 失败"); return 0; }
    int sig = WIFSTOPPED(status) ? WSTOPSIG(status) : -1;
    if (sigOut) *sigOut = sig;

    struct user_regs_struct after;
    unsigned long ret = 0;
    if (GetRegs(pid, &after) == 0) ret = after.rax;
    P("远调 fn=%lx 信号=%d 返回=0x%lx (rip=%lx rsp=%lx rax_old=%lx)", fn, sig, ret, after.rip, after.rsp, save.rax);

    // 恢复现场；进程仍处于 stop 态，pending 的哨兵信号在下次 CONT(data=0) 时被丢弃
    if (SetRegs(pid, &save) != 0) P("恢复寄存器失败");
    if (sig == SIGSEGV || sig == SIGTRAP || sig == SIGILL) {
        siginfo_t si;
        ptrace(PTRACE_GETSIGINFO, pid, 0, &si);
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
        // 路径后面必须是行尾或空白，避免 libc.so 命中 libc.so.6 之类的近似名
        char nxt = hit[wantLen];
        if (nxt != '\n' && nxt != ' ' && nxt != '\0' && nxt != '\r') continue;
        unsigned long s = 0, e = 0, off = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s %lx", &s, &e, perms, &off) != 4) continue;
        if (anyMin == 0 || s < anyMin) anyMin = s;
        if (off == 0) { if (best == 0 || s < best) best = s; }   // 文件偏移 0 段 = ELF 头 = 加载基址
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
    // 必须用完整路径匹配：进程里同时存在 houdini guest 库
    // (/system/vendor/lib64/arm64/libc.so) 与真正的 x86_64 库
    // (/apex/com.android.runtime/lib64/bionic/libc.so)，只按 basename 会命中错的那个。
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
    if (access(soPath, R_OK) != 0) { P("远端路径不可读: %s", strerror(errno)); return 1; }
    P("目标进程 %d, 模块 %s", pid, soPath);

    unsigned long pMmap   = 0, pDlopen = 0, pDlerror = 0;
    // 先附加再做符号解析（需要目标 maps）
    if (AttachAllThreads(pid) != 0) { P("附加失败"); return 1; }

    pMmap   = FindRemoteSym(pid, "libc.so", "mmap");
    pDlopen = FindRemoteSym(pid, "libdl.so", "dlopen");
    if (!pDlopen) pDlopen = FindRemoteSym(pid, "libc.so", "dlopen");
    pDlerror = FindRemoteSym(pid, "libdl.so", "dlerror");
    if (!pDlerror) pDlerror = FindRemoteSym(pid, "libc.so", "dlerror");
    if (!pMmap || !pDlopen) { P("符号解析失败 mmap=%lx dlopen=%lx", pMmap, pDlopen); ptrace(PTRACE_DETACH, pid, 0, 0); return 1; }

    int sig = 0;
    unsigned long rmem = RemoteCall(pid, pMmap, 0, 0x4000, MY_PROT_RWX,
                                    MAP_PRIVATE_ANON, (unsigned long)-1, 0, 0, &sig);
    if (!rmem || (long)rmem == -1) { P("远端 mmap 失败: %ld", (long)rmem); ptrace(PTRACE_DETACH, pid, 0, 0); return 1; }
    P("远端缓冲区 0x%lx", rmem);

    if (pt_write(pid, rmem, soPath, strlen(soPath) + 1) != 0) { P("写路径失败"); return 1; }
    char chk[256] = {0};
    pt_read(pid, rmem, chk, sizeof(chk));
    P("已写入路径: %s", chk);

    unsigned long h = RemoteCall(pid, pDlopen, rmem, RTLD_NOW_GLOBAL, 0, 0, 0, 0, rmem + 0x3000, &sig);
    P("dlopen 句柄 = 0x%lx", h);
    if (!h) {
        unsigned long e = RemoteCall(pid, pDlerror, 0, 0, 0, 0, 0, 0, rmem + 0x3000, &sig);
        if (e) { char msg[256] = {0}; pt_read(pid, e, msg, sizeof(msg) - 1); P("dlerror: %s", msg); }
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 1;
    }
    P("注入成功");

    // 全部线程脱离
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
