// sensor_native.cpp 鈥斺€?杩涚▼鍐呯洿璇?Cocos Creator 3.x 鍦烘櫙鍥撅紝浜у嚭 ESP 瀹炰綋
//
// 涓庢棫鐨?SensorNative 鐨勫尯鍒細
//   鏃х増渚濊禆 files/esp_layout.txt 鎵嬪伐鏍囧畾 director_ptr锛屼笖浣跨敤 Cocos 2.x 鍋忕Щ
//   锛坣ame@0x30 / children@0x88 / worldpos@0x140锛夛紝瀹為檯浠庢湭鍙敤銆?//   鏈増鍏ㄩ儴鍋忕Щ鏉ヨ嚜 IDA 瀵?libcocos.so 鐨勯潤鎬佺‘璁わ紙sub_37B8B0 = Node 鏋勯€犲嚱鏁帮級锛?//   鍩哄潃涓?vtable 杩愯鏃跺姩鎬佽В鏋愶紝鏃犻渶浠讳綍澶栭儴鏍囧畾鏂囦欢銆?//
// 鏁版嵁娴侊細libcocos base 鈫?CAM vtable 鎵爢 鈫?涓荤浉鏈?鈫?CAM_NODE 鈫?parent 涓婃函 鈫?Scene
//         鈫?DFS 鑺傜偣鏍?鈫?_worldPosition 鈫?鍘婚噸/杩囨护 鈫?Entity[]
//
// 鎬ц兘锛氱嫭绔嬮噰闆嗙嚎绋嬩互 ~120Hz(8.3ms) 浜у嚭蹇収锛屽啓鍙岀紦鍐诧紱娓叉煋绾跨▼鍙仛涓€娆?//       vector 鎷疯礉锛屾案涓嶉樆濉炲湪 pread 涓娿€傜浉鏈?Scene 鎸囬拡缂撳瓨 + 鍊欓€夊湴鍧€蹇〃锛?//       涓栫晫鏍瑰瓙鑺傜偣鏁扮粍缂撳瓨锛屽疄浣撳悕鍒嗙骇鍒锋柊锛?00ms锛夛紝鍘婚噸鏀逛负瀹氶暱鏁扮粍銆?
#include "sensor.h"
#include "config.h"
#include "log.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <set>
#include <array>
#include <algorithm>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/uio.h>
#include <pthread.h>
#include <mutex>
#include <atomic>
#include <errno.h>

#ifndef PKG_NAME
#define PKG_NAME "com.fmgame.pubg.aligames"
#endif

namespace {

// ---------------------------------------------------------------- 静态偏移
constexpr uint64_t VT_NODE_OFF  = 0x1511730ULL;   // cc::Node    vtable
constexpr uint64_t VT_SCENE_OFF = 0x1511840ULL;   // cc::Scene   vtable
constexpr uint64_t VT_CAM_OFF   = 0x150EA98ULL;   // cc::Camera  vtable

constexpr int OFF_NAME      = 0x38;   // std::string（libc++ alternate）
constexpr int OFF_PARENT    = 0x50;
constexpr int OFF_CHILD_BEG = 0x70;   // std::vector<Node*> begin
constexpr int OFF_CHILD_END = 0x78;   // std::vector<Node*> end
constexpr int OFF_LOCAL     = 0xEC;   // Mat4 _matrixLocal
constexpr int OFF_WORLDPOS  = 0x12C;  // Vec3 _worldPosition  (= 0xEC + 0x40)
constexpr int OFF_WORLDMAT  = 0x154;  // Mat4 _worldMatrix

constexpr int CAM_NODE     = 0x28;
constexpr int CAM_NEAR     = 0x60;
constexpr int CAM_FAR      = 0x64;
constexpr int CAM_VIEW     = 0xA0;
constexpr int CAM_PROJ     = 0xE0;
constexpr int CAM_VIEWPROJ = 0x160;
constexpr int CAM_POS      = 0x1F4;

// ---------------------------------------------------------------- 鍐呭瓨璇诲彇
struct MapEntry { uintptr_t start, end, off; char perms[8]; char name[192]; };
MapEntry g_maps[4096];
int  g_nm      = 0;
double g_mapsAt = -1e9;
int  g_memfd   = -1;

double Now() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int ReadMaps() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return g_nm;
    char line[640];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < 4096) {
        unsigned long s = 0, e = 0, o = 0;
        char perms[8] = {0}, name[192] = {0};
        int got = sscanf(line, "%lx-%lx %7s %lx %*s %*s %191[^\n]", &s, &e, perms, &o, name);
        if (got < 4) continue;
        g_maps[n].start = s; g_maps[n].end = e; g_maps[n].off = o;
        snprintf(g_maps[n].perms, sizeof(g_maps[n].perms), "%s", perms);
        char* q = name; while (*q == ' ') ++q;
        snprintf(g_maps[n].name, sizeof(g_maps[n].name), "%s", q);
        ++n;
    }
    fclose(f);
    g_nm = n;
    g_mapsAt = Now();
    return n;
}

bool InRange(uintptr_t a, size_t n) {
    for (int i = 0; i < g_nm; ++i)
        if (g_maps[i].perms[0] == 'r' && a >= g_maps[i].start && a + n <= g_maps[i].end) return true;
    return false;
}

// /proc/self/mem pread 瓒婄晫杩斿洖閿欒鑰屼笉鏄?SIGSEGV锛岃法 houdini guest 绌洪棿鍚屾牱鏈夋晥
// 读取统计（用于定位读失败来源）
std::atomic<long> g_vmOK{0}, g_vmBad{0}, g_fdOK{0}, g_fdBad{0}, g_fdReopen{0};

// 主路径：process_vm_readv 读自身进程，不占 fd、不受外部 close(fd) 干扰
static bool ReadVm(uintptr_t a, void* buf, size_t n) {
    struct iovec li{ buf, n }, ri{ (void*)a, n };
    ssize_t r = process_vm_readv(getpid(), &li, 1, &ri, 1, 0);
    if (r == (ssize_t)n) { ++g_vmOK; return true; }
    ++g_vmBad;
    return false;
}

// 回退路径：/proc/self/mem pread。若 fd 被外部误关（实测会出现 EBADF），自动重开重试
static bool ReadByFd(uintptr_t a, void* buf, size_t n) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (g_memfd >= 0 && fcntl(g_memfd, F_GETFD) < 0) { close(g_memfd); g_memfd = -1; }
        if (g_memfd < 0) {
            g_memfd = open("/proc/self/mem", O_RDONLY);
            if (g_memfd >= 0) ++g_fdReopen;
            else return false;
        }
        size_t got = 0;
        while (got < n) {
            ssize_t r = pread(g_memfd, (char*)buf + got, n - got, (off_t)(a + got));
            if (r <= 0) break;
            got += (size_t)r;
        }
        if (got == n) { ++g_fdOK; return true; }
        // 疑似 fd 失效：关掉重开一次
        close(g_memfd);
        g_memfd = -1;
    }
    ++g_fdBad;
    return false;
}

bool ReadMem(uintptr_t a, void* buf, size_t n) {
    if (n == 0) return true;
    if (ReadVm(a, buf, n)) return true;
    return ReadByFd(a, buf, n);
}

template <class T>
inline bool Rd(uintptr_t a, T& v) { return ReadMem(a, &v, sizeof(T)); }

inline bool RdF(uintptr_t a, float* v, size_t n) { return ReadMem(a, v, n * 4); }

// libc++ alternate std::string
bool ReadStdString(uintptr_t p, std::string& out) {
    uint8_t hdr[24];
    if (!ReadMem(p, hdr, sizeof(hdr))) return false;
    const uint8_t flag = hdr[0];
    if (flag & 1) {
        uint64_t sz = 0, dp = 0;
        memcpy(&sz, hdr + 8, 8); memcpy(&dp, hdr + 16, 8);
        if (sz == 0 || sz > 96) return false;
        char bf[97] = {0};
        if (!ReadMem((uintptr_t)dp, bf, (size_t)sz)) return false;
        for (size_t i = 0; i < sz; ++i) {
            unsigned char c = (unsigned char)bf[i];
            if (c < 0x20 || c == 0x7F) return false;
        }
        out.assign(bf, (size_t)sz);
        return true;
    }
    size_t sz = (size_t)(flag >> 1);
    if (sz == 0 || sz > 22) return false;
    out.assign((const char*)hdr + 1, sz);
    for (size_t i = 0; i < sz; ++i) {
        unsigned char c = (unsigned char)out[i];
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

void Mat4Mul(const float* a, const float* b, float* o) {
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int rw = 0; rw < 4; ++rw) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + rw] * b[c * 4 + k];
            r[c * 4 + rw] = s;
        }
    memcpy(o, r, 64);
}

// ---------------------------------------------------------------- 状态
struct NodeInfo {
    uintptr_t addr = 0;
    uintptr_t parent = 0;
    int depth = 0;
    float world[3] = {0, 0, 0};
};

uintptr_t g_base     = 0;
uintptr_t g_scene    = 0;
uintptr_t g_cam      = 0;
uintptr_t g_camNode  = 0;
uintptr_t g_worldRoot = 0;   // 3D 涓栫晫鏍?= 鐩告満涓婃函鍒?Scene 鐨勭洿鎺ュ瓙鑺傜偣
uint64_t  g_nodeVT   = 0, g_sceneVT = 0, g_camVT = 0;
double    g_scanAt   = -1e9;
double    g_snapAt   = -1e9;
float     g_camPos[3] = {0, 0, 0};
float     g_viewProj[16] = {0};
bool      g_ready = false;

// ---- 鐩告満鍊欓€夊揩琛紙鎵弿鏃堕『甯︾櫥璁帮紝澶辨晥鏃跺厛楠岃繖鍑犱釜鍦板潃锛岄伩鍏嶆暣鍫嗛噸鎵級----
constexpr int MAX_CAM_CAND = 128;
uintptr_t g_camCands[MAX_CAM_CAND] = {0};
int       g_nCamCands = 0;

// ---- 涓栫晫鏍瑰瓙鑺傜偣鏁扮粍缂撳瓨锛坈ontent 鍙湪 begin/end 鍙樺寲鏃堕噸璇伙級----
std::vector<uintptr_t> g_kids;
uintptr_t g_kidsBeg = 0, g_kidsEnd = 0;

// ---- 瀹炰綋鍚嶇紦瀛橈紙姣?NAME_TTL 绉掑埛鏂颁竴娆★紝鍙渷鎺夋瘡甯т竴娆?ReadStdString锛?---
constexpr int MAX_NAME_CACHE = 512;
constexpr double NAME_TTL = 0.4;
struct NameSlot { uintptr_t addr; double at; char name[40]; };
NameSlot g_names[MAX_NAME_CACHE];
int      g_nNames = 0;

// ---- 蹇収鍙岀紦鍐?----
struct Snap {
    std::vector<Entity> ents;
    ViewInfo view;
    double   at = 0;          // 閲囬泦瀹屾垚鏃跺埢
    uint64_t seq = 0;
};
std::mutex       g_snapMtx;
Snap             g_pendSnap;   // 采集线程私有写
Snap             g_readySnap;  // 渲染线程读
std::atomic<uint64_t> g_snapSeq{0};
std::atomic<bool>     g_workerRun{false};
std::atomic<bool>     g_workerAlive{false};
pthread_t        g_workerTh{};
std::atomic<float>    g_collectMs{0.f};   // 鍗曟閲囬泦鑰楁椂锛堟绉掞級

constexpr int MAX_NODES  = 24000;
constexpr int MAX_DEPTH  = 16;
constexpr int MAX_ENT    = 900;
constexpr double RESCAN_COOLDOWN = 1.5;   // 閲嶆壂鏈€灏忛棿闅旓紙绉掞級
constexpr double SNAP_INTERVAL   = 1.0 / 120.0;
constexpr double SNAP_INTERVAL_FAST = 1.0 / 240.0;
constexpr double SNAP_INTERVAL_SLOW = 1.0 / 90.0;   // 閲囬泦鍛ㄦ湡锛?20Hz
constexpr double MAPS_TTL        = 10.0;          // /proc/self/maps 鍒锋柊闂撮殧

uintptr_t FindCocosBase() {
    uintptr_t base = 0;
    for (int i = 0; i < g_nm; ++i)
        if (strstr(g_maps[i].name, "libcocos.so") && g_maps[i].off == 0) {
            if (!base || g_maps[i].start < base) base = g_maps[i].start;
        }
    return base;
}

// 主相机判据：透视投影 proj[10]≈-1 且 proj[11]≈-1，pos 非零，far 足够大
bool EvalCamera(uintptr_t cam, float* projOut, float* posOut, float* nearOut, float* farOut) {
    float proj[16];
    if (!RdF(cam + CAM_PROJ, proj, 16)) return false;
    if (fabsf(proj[10] + 1.0f) > 0.02f || fabsf(proj[11] + 1.0f) > 0.02f) return false;
    float pos[3] = {0, 0, 0};
    if (!RdF(cam + CAM_POS, pos, 3)) return false;
    if (fabsf(pos[0]) < 0.01f && fabsf(pos[1]) < 0.01f && fabsf(pos[2]) < 0.01f) return false;
    float nf = 0.f, ff = 0.f;
    Rd(cam + CAM_NEAR, nf); Rd(cam + CAM_FAR, ff);
    if (!(ff > 10.f) || nf < 0.f || ff <= nf) return false;
    if (projOut) memcpy(projOut, proj, 64);
    if (posOut)  memcpy(posOut, pos, 12);
    if (nearOut) *nearOut = nf;
    if (farOut)  *farOut = ff;
    return true;
}

static size_t ChildCount(uintptr_t node, uintptr_t& beg);

// 在 Scene 的直接子节点里挑「带部件(1..16 个子节点)且子节点数最多」的那个作为世界根。
// 相机常常直接挂在 Scene 下，这时 wroot 会等于 camNode 本身，必须另找承载实体的节点。
uintptr_t PickWorldRoot(uintptr_t scene, uintptr_t exclude) {
    uintptr_t cb = 0;
    size_t n = ChildCount(scene, cb);
    if (!n || n > 256) return 0;
    std::vector<uintptr_t> kids(n);
    if (!ReadMem(cb, kids.data(), n * 8)) return 0;
    uintptr_t best = 0;
    size_t bestN = 0;
    for (size_t i = 0; i < n; ++i) {
        uintptr_t k = kids[i];
        if (!k || k == exclude) continue;
        uint64_t vt = 0;
        if (!Rd(k, vt) || vt != g_nodeVT) continue;
        uintptr_t kb = 0;
        size_t nc = ChildCount(k, kb);
        if (nc < 1 || nc > 64) continue;
        if (nc > bestN) { bestN = nc; best = k; }
    }
    return best;
}

// 由相机指针解析 node/scene/worldRoot（相机候选快表复用此函数）
bool ResolveChain(uintptr_t cam) {
    uintptr_t cnode = 0;
    if (!Rd(cam + CAM_NODE, cnode) || !cnode) return false;
    uint64_t vt = 0;
    if (!Rd(cnode, vt)) return false;

    uintptr_t cur = cnode, scene = 0, wroot = 0;
    for (int lvl = 0; lvl < 64 && cur; ++lvl) {
        if (!Rd(cur, vt)) break;
        if (vt == g_sceneVT) { scene = cur; break; }
        if (vt != g_nodeVT) break;
        uintptr_t par = 0;
        if (!Rd(cur + OFF_PARENT, par)) break;
        if (par == cur) break;
        wroot = cur;               // 鑻ヤ笅涓€璺虫槸 Scene锛屽垯 cur 鍗充笘鐣屾牴
        cur = par;
    }
    if (!scene) return false;

    // 相机直接挂在 Scene 下 -> wroot == cnode，此时另外挑真正的 3D 世界根
    if (wroot == 0 || wroot == cnode) {
        uintptr_t alt = PickWorldRoot(scene, cnode);
        if (alt) wroot = alt;
        else if (!wroot) wroot = scene;
    }

    if (wroot != g_worldRoot) { g_kids.clear(); g_kidsBeg = g_kidsEnd = 0; }
    g_cam = cam; g_camNode = cnode; g_scene = scene; g_worldRoot = wroot;
    return true;
}

// 先用候选快表重定位相机（几次 pread 搞定），全部失效才回到整堆扫描
bool TryCamCands() {
    for (int i = 0; i < g_nCamCands; ++i) {
        uintptr_t cam = g_camCands[i];
        if (!cam) continue;
        uint64_t vt = 0;
        if (!Rd(cam, vt) || vt != g_camVT) continue;
        if (!EvalCamera(cam, nullptr, nullptr, nullptr, nullptr)) continue;
        if (ResolveChain(cam)) {
            LOGI("SensorScene: 鍊欓€夊揩琛ㄥ懡涓?cam=%p worldRoot=%p", (void*)cam, (void*)g_worldRoot);
            return true;
        }
    }
    return false;
}

// 鍏ㄥ爢鎵?CAM vtable锛屽彇绗竴涓悎鏍肩殑閫忚鐩告満锛涘悓鏃堕『甯﹁В鏋愬嚭 Scene
bool ScanCamera() {
    g_cam = 0; g_camNode = 0; g_scene = 0;
    static uint8_t buf[1 << 20];
    int camsFound = 0, candOk = 0;
    int segs = 0, nodeHits = 0;
    unsigned long long scanned = 0;
    int failCnt = 0, firstErr = 0;
    uintptr_t firstFail = 0;
    g_nCamCands = 0;

    for (int m = 0; m < g_nm && camsFound < 256; ++m) {
        if (g_maps[m].perms[0] != 'r' || g_maps[m].perms[1] != 'w') continue;
        if (strstr(g_maps[m].name, "libcocos.so")) continue;       // 璺宠繃浠ｇ爜/鏁版嵁鏈綋
        uintptr_t s = g_maps[m].start, e = g_maps[m].end;
        if (e - s > 384ULL * 1024 * 1024) continue;
        ++segs;

        for (uintptr_t q = s; q < e; q += (1 << 20)) {
            size_t chunk = (e - q < (1ULL << 20)) ? (size_t)(e - q) : (size_t)(1 << 20);
            if (!ReadMem(q, buf, chunk)) {
                if (!failCnt) { firstFail = q; firstErr = errno; }
                ++failCnt;
                continue;
            }
            scanned += chunk;
            for (size_t o = 0; o + 8 <= chunk; o += 8) {
                uint64_t v = 0;
                memcpy(&v, buf + o, 8);
                if (v == g_nodeVT) ++nodeHits;                     // 璇婃柇锛歂ode 瀵硅薄鑳藉惁鎵埌
                if (v != g_camVT) continue;
                ++camsFound;
                uintptr_t cam = q + o;
                if (!EvalCamera(cam, nullptr, nullptr, nullptr, nullptr)) continue;
                ++candOk;
                if (g_nCamCands < MAX_CAM_CAND) g_camCands[g_nCamCands++] = cam;
                if (!ResolveChain(cam)) continue;
                LOGI("SensorScene: 鎵弿鍛戒腑 cam=%p node=%p scene=%p worldRoot=%p (鎵弿 %d 涓?cam 鍊欓€? %d 涓彲鐢?",
                     (void*)cam, (void*)g_camNode, (void*)g_scene, (void*)g_worldRoot, camsFound, candOk);
                return true;
            }
        }
    }
    // 鏈懡涓棩蹇楅檺棰戯紙鍘熺増 120Hz 閲囬泦涓嬩細姣忕鍒蜂笂鐧炬潯锛宭ogcat 鏈韩灏辨槸寤惰繜婧愶級
    static double s_warnAt = 0;
    const double nowW = Now();
    if (nowW - s_warnAt > 2.0) {
        s_warnAt = nowW;
        LOGW("SensorScene: no main camera (maps %d segs / scan %d segs / %.1f MB, camVT hit %d, nodeVT hit %d, persp ok %d, camVT=%p nodeVT=%p | readfail %d, first %p errno=%d, memfd=%d, vmOK %ld vmBad %ld fdOK %ld fdBad %ld reopen %ld)",
             g_nm, segs, (double)scanned / 1048576.0, camsFound, nodeHits, candOk,
             (void*)g_camVT, (void*)g_nodeVT, failCnt, (void*)firstFail, firstErr, g_memfd,
             g_vmOK.load(), g_vmBad.load(), g_fdOK.load(), g_fdBad.load(), g_fdReopen.load());
    }
    return false;
}

// 缂撳瓨鏍￠獙锛氫笁涓叧閿璞＄殑 vtable 浠嶅湪鍒欑户缁敤锛涘け鏁堟椂鍏堢敤鍊欓€夊揩琛ㄧ绾ч噸瀹氫綅
bool CacheValid() {
    if (!g_cam || !g_scene) return TryCamCands();
    uint64_t v = 0;
    if (!Rd(g_cam, v) || v != g_camVT) return TryCamCands();
    if (!Rd(g_scene, v) || v != g_sceneVT) return TryCamCands();
    if (!EvalCamera(g_cam, nullptr, nullptr, nullptr, nullptr)) return TryCamCands();
    if (!g_worldRoot) return TryCamCands();
    return true;
}

bool EnsureReady() {
    const double now = Now();
    if (now - g_mapsAt > MAPS_TTL || g_nm == 0) {
        ReadMaps();
        uintptr_t b = FindCocosBase();
        if (b && b != g_base) {
            g_base = b;
            g_nodeVT  = b + VT_NODE_OFF;
            g_sceneVT = b + VT_SCENE_OFF;
            g_camVT   = b + VT_CAM_OFF;
            LOGI("SensorScene: libcocos base=%lx nodeVT=%lx sceneVT=%lx camVT=%lx",
                 (unsigned long)g_base, (unsigned long)g_nodeVT,
                 (unsigned long)g_sceneVT, (unsigned long)g_camVT);
            g_cam = g_scene = g_camNode = g_worldRoot = 0;
            g_nCamCands = 0;
            g_kids.clear(); g_kidsBeg = g_kidsEnd = 0;
        } else if (!b) {
            return false;
        }
    }
    if (CacheValid()) return true;
    if (now - g_scanAt < RESCAN_COOLDOWN) return false;
    g_scanAt = now;
    if (!ScanCamera()) return false;
    g_ready = true;
    return true;
}

// 鍙栬妭鐐逛笘鐣屽潗鏍囷細浼樺厛鐩磋 _worldPosition锛屽叏闆?寮傚父鏃跺洖閫€鐖堕摼绱箻 local
struct WorldCtx {
    std::vector<NodeInfo> nodes;
    std::set<uintptr_t>   seen;
    int count = 0;
};

bool ReadWorldPos(uintptr_t node, const float* pw, float* out) {
    // 实测结论：Node 上 +0x12C / +0x154 取到的不是有效世界坐标（父子呈 2 倍等假值），
    // 唯一可信来源是父链累乘 _matrixLocal(0xEC)，由调用方预算好放进 pw。
    (void)node;
    if (fabsf(pw[12]) + fabsf(pw[13]) + fabsf(pw[14]) < 0.0001f) return false;
    out[0] = pw[12]; out[1] = pw[13]; out[2] = pw[14];
    return true;
}

// ---------------------------------------------------------------- 鍊欓€夊鍑?// 涓€娆℃€ф妸銆屾墍鏈夊彲鑳芥槸瀹炰綋鐨勮妭鐐广€嶈惤鍒版枃浠讹紝渚涗汉鐪兼牳瀵瑰摢涓€鎵规槸鐜╁
void DumpCandidates(const std::vector<Entity>& list, uintptr_t scene) {
    char path[256];
    snprintf(path, sizeof(path), "/data/data/%s/files/esp_cand.txt", PKG_NAME);
    FILE* f = fopen(path, "w");
    if (!f) f = fopen("/sdcard/esp_cand.txt", "w");
    if (!f) { LOGW("候选导出失败: 无法打开 %s", path); return; }
    fprintf(f, "# cam=(%.2f, %.2f, %.2f) scene=%p count=%zu\n",
            g_camPos[0], g_camPos[1], g_camPos[2], (void*)scene, list.size());
    fprintf(f, "# idx addr              name                          wp                          dist\n");
    for (size_t i = 0; i < list.size(); ++i) {
        const Entity& e = list[i];
        fprintf(f, "%3zu %016llx  %-28s (%10.2f,%9.2f,%10.2f) %8.2f\n",
                i, (unsigned long long)e.id, e.name,
                e.feet.x, e.feet.y, e.feet.z, e.distance);
    }
    fclose(f);
    LOGI("鍊欓€夊鍑? %s (%zu 鏉?", path, list.size());
}

// 鍙栧瓙鑺傜偣鏁扮粍
static size_t ChildCount(uintptr_t node, uintptr_t& beg) {
    uintptr_t cb = 0, ce = 0;
    if (!Rd(node + OFF_CHILD_BEG, cb) || !Rd(node + OFF_CHILD_END, ce)) return 0;
    if (!cb || ce <= cb || ((ce - cb) % 8) != 0 || (ce - cb) > 8u * 8000u) return 0;
    beg = cb;
    return (size_t)((ce - cb) / 8);
}

static bool SelfMatrix(uintptr_t node, const float* parentM, float* out) {
    float lm[16];
    if (!RdF(node + OFF_LOCAL, lm, 16)) return false;
    Mat4Mul(parentM, lm, out);
    return true;
}

// 坐标诊断：把三种可能来源并排落盘，一次确认哪个是真世界坐标
static void DumpMatrixProbe(uintptr_t node, const float* selfM) {
    static int seq = 0;
    if (seq > 2) return;
    ++seq;
    char path[256];
    snprintf(path, sizeof(path), "/data/data/%s/files/esp_mtx.txt", PKG_NAME);
    FILE* f = fopen(path, seq == 1 ? "w" : "a");
    if (!f) return;
    if (seq == 1)
        fprintf(f, "# cam=(%.2f,%.2f,%.2f)\n"
                   "# name            selfM[12..14]             off12C                    off154\n",
                g_camPos[0], g_camPos[1], g_camPos[2]);
    float a[3] = {0,0,0}, b[3] = {0,0,0};
    RdF(node + OFF_WORLDPOS, a, 3);
    RdF(node + OFF_WORLDMAT + 12, b, 3);
    std::string nm; ReadStdString(node + OFF_NAME, nm);
    fprintf(f, "%-16s (%9.3f,%9.3f,%9.3f)  (%9.3f,%9.3f,%9.3f)  (%9.3f,%9.3f,%9.3f)\n",
            nm.c_str(), selfM[12], selfM[13], selfM[14], a[0], a[1], a[2], b[0], b[1], b[2]);
    fclose(f);
}

// 瀹炰綋鍚嶅垎绾х紦瀛橈細鍚屼竴鍦板潃 400ms 鍐呬笉閲嶅瑙?std::string锛堝師鏈瘡甯ф瘡瀹炰綋涓€娆★級
void CachedName(uintptr_t addr, char* out, size_t cap) {
    out[0] = 0;
    const double now = Now();
    for (int i = 0; i < g_nNames; ++i) {
        if (g_names[i].addr != addr) continue;
        if (now - g_names[i].at >= NAME_TTL) {
            std::string nm;
            if (ReadStdString(addr + OFF_NAME, nm))
                snprintf(g_names[i].name, sizeof(g_names[i].name), "%s", nm.c_str());
            g_names[i].at = now;
        }
        snprintf(out, cap, "%s", g_names[i].name);
        return;
    }
    static int rr = 0;
    NameSlot* slot = (g_nNames < MAX_NAME_CACHE) ? &g_names[g_nNames++]
                                                 : &g_names[(rr++) % MAX_NAME_CACHE];
    slot->addr = addr; slot->at = now; slot->name[0] = 0;
    std::string nm;
    if (ReadStdString(addr + OFF_NAME, nm))
        snprintf(slot->name, sizeof(slot->name), "%s", nm.c_str());
    snprintf(out, cap, "%s", slot->name);
}

void Collect(uintptr_t root, std::vector<Entity>& out) {
    if (!root) return;

    const float maxDist = (g_cfg.maxDistance > 0.f) ? g_cfg.maxDistance : 400.f;
    const float limit   = std::max(1200.f, maxDist * 2.5f);

    // 涓栫晫鏍硅嚜韬殑涓栫晫鐭╅樀
    float rootM[16];
    {
        uint64_t vt = 0;
        if (!Rd(root, vt) || vt != g_nodeVT) return;
        float lm[16];
        if (!RdF(root + OFF_LOCAL, lm, 16)) return;
        memcpy(rootM, lm, 64);              // 世界根的 local 即世界矩阵
    }

    // 瀛愯妭鐐规暟缁勭紦瀛橈細root/begin/涓暟 閮戒笉鍙樻墠澶嶇敤锛岀渷鎺夋瘡甯т竴娆?n*8 瀛楄妭 pread
    static uintptr_t s_kidsRoot = 0;
    uintptr_t cb = 0;
    size_t n = ChildCount(root, cb);
    if (!n) return;
    if (root != s_kidsRoot || cb != g_kidsBeg || n != g_kids.size()) {
        std::vector<uintptr_t> tmp(n);
        if (!ReadMem(cb, tmp.data(), n * 8)) return;   // 澶辫触淇濈暀鏃х紦瀛橈紝涓嬭疆閲嶈
        g_kids.swap(tmp);
        g_kidsBeg = cb;
        s_kidsRoot = root;
    }

    // 瀹氶暱鍘婚噸琛紙鏇夸唬 std::set锛岀渷鎺夋瘡鏉″疄浣撶殑绾㈤粦鏍戝垎閰嶏級
    std::array<std::array<int, 3>, 1024> dedup{};
    int nDedup = 0;
    auto isDup = [&](int a, int b, int c) -> bool {
        for (int i = 0; i < nDedup; ++i)
            if (dedup[i][0] == a && dedup[i][1] == b && dedup[i][2] == c) return true;
        if (nDedup < 1024) dedup[nDedup++] = {a, b, c};
        return false;
    };

    const float cy = g_camPos[1];
    int visited = 0, withKids = 0;
    int fMat = 0, fZero = 0, fFar = 0, fDup = 0, fDist = 0;
    // velocity diff table (keyed by node addr)
    static uint64_t pvId[256];
    static float    pvP[256][3];
    static double   pvT[256];
    static int      pvN = 0;
    const double    nowT = Now();

    for (size_t i = 0; i < n && out.size() < MAX_ENT; ++i) {
        uintptr_t k = g_kids[i];
        if (!k) continue;
        uint64_t vt = 0;
        if (!Rd(k, vt) || vt != g_nodeVT) continue;
        ++visited;

        uintptr_t kb = 0;
        size_t nc = ChildCount(k, kb);
        if (nc < 1 || nc > 16) continue;    // 瀹炰綋瀹瑰櫒蹇呭甫閮ㄤ欢/楠ㄩ
        ++withKids;

        float selfM[16];
        if (!SelfMatrix(k, rootM, selfM)) { ++fMat; continue; }

        // 鐪熶笘鐣屽潗鏍?= 鑷繁绠楀嚭鐨勪笘鐣岀煩闃靛钩绉诲垪锛堜慨澶嶇偣锛氭棫鐗堣浼犵埗鐭╅樀瀵艰嚧鏁翠綋鍋忕Щ涓€灞傦級
        const float x = selfM[12], y = selfM[13], z = selfM[14];
        if (fabsf(x) + fabsf(y) + fabsf(z) < 1e-3f) { ++fZero; continue; }

        const float dx = x - g_camPos[0], dy = y - cy, dz = z - g_camPos[2];
        const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        if (dist < 0.3f || dist > limit) { ++fDist; continue; }
        if (fabsf(dy) > 200.f) { ++fFar; continue; }

        std::array<int, 3> key{
            (int)lroundf(x * 4.f), (int)lroundf(y * 4.f), (int)lroundf(z * 4.f)};
        if (isDup(key[0], key[1], key[2])) { ++fDup; continue; }

        if (g_cfg.matrixProbe && out.size() < 3) DumpMatrixProbe(k, selfM);

        Entity e;
        e.id       = (uint64_t)k;
        e.feet     = Vec3(x, y, z);
        e.chest    = Vec3(x, y + 1.15f, z);
        e.pelvis   = Vec3(x, y + 0.85f, z);
        e.head     = Vec3(x, y + 1.70f, z);
        e.distance = dist;
        e.visible  = true;
        {
            int slot = -1;
            for (int q = 0; q < pvN; ++q) if (pvId[q] == (uint64_t)k) { slot = q; break; }
            Vec3 vel(0.f, 0.f, 0.f);
            if (slot >= 0) {
                const double dtv = nowT - pvT[slot];
                if (dtv > 0.0015 && dtv < 0.5) {
                    vel = Vec3((float)((x - pvP[slot][0]) / dtv),
                               (float)((y - pvP[slot][1]) / dtv),
                               (float)((z - pvP[slot][2]) / dtv));
                    const float sp = vel.Length();
                    if (sp > 40.f) { const float k2 = 40.f / sp; vel = vel * k2; }
                    else if (sp < 0.02f) vel = Vec3(0.f, 0.f, 0.f);
                }
            } else if (pvN < 256) {
                slot = pvN++; pvId[slot] = (uint64_t)k;
            }
            if (slot >= 0) {
                pvP[slot][0] = x; pvP[slot][1] = y; pvP[slot][2] = z; pvT[slot] = nowT;
            }
            e.velocity = vel;
        }
        e.hasBones = (nc >= 3);
        CachedName(k, e.name, sizeof(e.name));
        out.push_back(e);
    }

    // 鍊欓€夊鍑哄彧鍦ㄩ渶瑕佹帓鏌ユ椂鍋氾紝涓斿彧鍋氬墠涓よ疆锛堝師鏉ユ瘡 4/24/60 杞兘浼氬啓鐩?+ 鎵撴棩蹇楋級
    static int snapSeq = 0;
    ++snapSeq;
    if (snapSeq <= 2) {
        DumpCandidates(out, root);
    }
    if (snapSeq == 2) {
        LOGI("SensorScene: worldRoot=%p kids=%zu withKids=%d ents=%zu | drop matFail=%d zero=%d dist=%d dy=%d dup=%d | limit=%.0f camY=%.1f cam=(%.1f,%.1f,%.1f)",
             (void*)root, n, withKids, out.size(), fMat, fZero, fDist, fFar, fDup, limit, cy,
             g_camPos[0], g_camPos[1], g_camPos[2]);
    }
}

// ---------------------------------------------------------------- 閲囬泦绾跨▼
// 单次采集：相机位姿 + viewProj + 世界根实体列表，全部写进 s（不碰渲染侧数据）
bool DoOnce(Snap& s) {
    if (!EnsureReady()) return false;

    float cp[3];
    if (!RdF(g_cam + CAM_POS, cp, 3)) return false;
    float vp[16];
    if (!RdF(g_cam + CAM_VIEWPROJ, vp, 16)) return false;

    bool ident = true;
    for (int i = 0; i < 16 && ident; ++i) {
        const float want = (i % 5 == 0) ? 1.f : 0.f;
        if (fabsf(vp[i] - want) > 0.0001f) ident = false;
    }
    if (ident) return false;      // 鍗曚綅鐭╅樀 = 鐩告満灏氭湭绠楀嚭 viewProj

    memcpy(g_camPos, cp, 12);
    memcpy(g_viewProj, vp, 16 * 4);

    s.ents.clear();
    Collect(g_worldRoot ? g_worldRoot : g_scene, s.ents);

    memset(&s.view.viewProj, 0, sizeof(s.view.viewProj));
    memcpy(s.view.viewProj.m, vp, 64);
    s.view.camPos    = Vec3(cp[0], cp[1], cp[2]);
    s.view.frameTime = Now();
    s.view.viewW     = (g_cfg.screenW > 0) ? g_cfg.screenW : 1920;
    s.view.viewH     = (g_cfg.screenH > 0) ? g_cfg.screenH : 1080;
    if (s.view.fovY <= 0.f || s.view.fovY > 179.f) s.view.fovY = 60.f;
    s.view.valid     = true;
    return true;
}

static double SnapInterval() {
    if (g_cfg.workerHz >= 60) return 1.0 / (double)g_cfg.workerHz;
    const float ms = g_collectMs.load();
    if (ms < 3.0f)  return SNAP_INTERVAL_FAST;   // 采集够快就上 240Hz
    if (ms < 8.0f)  return SNAP_INTERVAL;        // 常规 120Hz
    return SNAP_INTERVAL_SLOW;                   // 采集慢了降到 90Hz，别堆积
}

void* WorkerMain(void*) {
    double next = Now();
    uint64_t seq = 0;
    double lastLog = Now();
    int failStreak = 0;
    while (g_workerRun.load(std::memory_order_relaxed)) {
        const double t0 = Now();
        if (DoOnce(g_pendSnap)) {
            g_pendSnap.at  = Now();
            g_pendSnap.seq = ++seq;
            {
                std::lock_guard<std::mutex> lk(g_snapMtx);
                std::swap(g_readySnap.ents, g_pendSnap.ents);   // 闆跺垎閰嶄氦鎹紝娓叉煋渚ц鐨勬槸绋冲畾鍓湰
                g_readySnap.view = g_pendSnap.view;
                g_readySnap.at   = g_pendSnap.at;
                g_readySnap.seq  = g_pendSnap.seq;
            }
            g_snapSeq.store(seq);
            g_collectMs.store((float)((Now() - t0) * 1000.0));
            failStreak = 0;
        } else {
            if (failStreak++ == 0) LOGW("SensorWorker: no valid data (camera not ready / scene empty)");
        }
        if (Now() - lastLog > 5.0) {
            lastLog = Now();
            LOGI("SensorWorker: seq=%llu 閲囬泦鑰楁椂 %.2f ms 瀹炰綋 %zu 绾跨▼娲?%d",
                 (unsigned long long)seq, (double)g_collectMs.load(),
                 (size_t)([&]{ std::lock_guard<std::mutex> lk(g_snapMtx); return g_readySnap.ents.size(); })(),
                 (int)g_workerAlive.load());
        }
        // 涓ユ牸鎸?8.3ms 鑺傛媿鎺ㄨ繘锛岃惤鍚庝笉杩斤紙閬垮厤鍫嗙Н锛?        next += SnapInterval();
        const double now = Now();
        if (next < now) next = now;
        const double slp = next - now;
        if (slp > 0.0004) {
            struct timespec ts{};
            ts.tv_sec  = (time_t)slp;
            ts.tv_nsec = (long)((slp - (double)ts.tv_sec) * 1e9);
            nanosleep(&ts, nullptr);
        }
    }
    g_workerAlive.store(false);
    LOGI("SensorWorker: thread exit");
    return nullptr;
}

void StartWorker() {
    if (g_workerRun.exchange(true)) return;
    g_workerAlive.store(true);
    if (pthread_create(&g_workerTh, nullptr, WorkerMain, nullptr) == 0) {
        LOGI("SensorWorker: started, period %.1f ms (120Hz)", SNAP_INTERVAL * 1000.0);
    } else {
        g_workerRun.store(false);
        g_workerAlive.store(false);
        LOGW("SensorWorker: 绾跨▼鍒涘缓澶辫触锛屽洖閫€鍚屾閲囬泦");
    }
}

void StopWorker() {
    if (!g_workerRun.exchange(false)) return;
    pthread_join(g_workerTh, nullptr);
    LOGI("SensorWorker: stopped");
}

} // namespace

// ================================================================ 瀵瑰瀹炵幇
class SensorScene : public ISensor {
public:
    const char* Name() const override { return "Cocos3.x SceneGraph(auto)"; }

    bool Init() override {
        ReadMaps();
        StartWorker();             // 采集常驻线程：即便 base 暂未就绪也会在内部重试
        uintptr_t b = FindCocosBase();
        if (!b) {
            LOGW("SensorScene: 鏈壘鍒?libcocos.so 鏄犲皠锛岄噰闆嗙嚎绋嬩細鎸佺画閲嶈瘯");
            return false;          // 不阻塞
        }
        g_base    = b;
        g_nodeVT  = b + VT_NODE_OFF;
        g_sceneVT = b + VT_SCENE_OFF;
        g_camVT   = b + VT_CAM_OFF;
        LOGI("SensorScene: 鍒濆鍖?base=%lx", (unsigned long)b);
        return true;
    }

    bool Update(ViewInfo& view, std::vector<Entity>& out) override {
        out.clear();
        view.valid = false;

        // 渲染线程只做一次 vector 拷贝，pread / 遍历全在采集线程里完成
        {
            std::lock_guard<std::mutex> lk(g_snapMtx);
            if (g_readySnap.seq == 0 || !g_readySnap.view.valid) {
                if (!g_workerRun.load()) StartWorker();
                return false;
            }
            out = g_readySnap.ents;
            view = g_readySnap.view;
            lastEnts_     = (int)out.size();
            lastSnapshot_ = g_readySnap.at;

            // (1) live camera refresh: snapshot lag shows up as boxes not tracking view rotation.
            //     Re-read 64B viewProj + 12B camPos on render thread, cost is microseconds.
            if (g_cfg.btLiveCamera && g_cam) {
                float vp[16], cp[3];
                if (RdF(g_cam + CAM_VIEWPROJ, vp, 16) && RdF(g_cam + CAM_POS, cp, 3)) {
                    bool ident = true;
                    for (int i = 0; i < 16 && ident; ++i) {
                        const float want = (i % 5 == 0) ? 1.f : 0.f;
                        if (fabsf(vp[i] - want) > 0.0001f) ident = false;
                    }
                    if (!ident) {
                        memcpy(view.viewProj.m, vp, 64);
                        view.camPos = Vec3(cp[0], cp[1], cp[2]);
                    }
                }
            }

            // (2) velocity extrapolation: push entity positions to "now" to kill snapshot lag
            if (g_cfg.predEsp && !out.empty()) {
                const double lagRaw = Now() - g_readySnap.at;
                const float  lag    = (float)std::min(lagRaw, (double)g_cfg.predLagMax);
                if (lag > 0.0005f) {
                    for (size_t i = 0; i < out.size(); ++i) {
                        Entity& e = out[i];
                        const Vec3 d = e.velocity * lag;
                        if (fabsf(d.x) + fabsf(d.y) + fabsf(d.z) < 1e-5f) continue;
                        e.head = e.head + d; e.chest = e.chest + d;
                        e.pelvis = e.pelvis + d; e.feet = e.feet + d;
                        for (int bi = 0; bi < BoneIdx::Count; ++bi) e.bones[bi] = e.bones[bi] + d;
                    }
                }
            }
        }
        return !out.empty();
    }

    void Dump() override {
        LOGI("SensorScene: base=%lx scene=%p cam=%p ents=%d",
             (unsigned long)g_base, (void*)g_scene, (void*)g_cam, lastEnts_);
        if (!g_scene) return;
        uintptr_t cb = 0, ce = 0;
        Rd(g_scene + OFF_CHILD_BEG, cb); Rd(g_scene + OFF_CHILD_END, ce);
        LOGI("SensorScene: scene children = %zu",
             (ce > cb && ((ce - cb) % 8) == 0) ? (size_t)((ce - cb) / 8) : 0);
    }

private:
    std::vector<Entity> lastList_;
    int    lastEnts_ = 0;
    double lastSnapshot_ = 0;
};

ISensor* CreateSceneSensor() { return new SensorScene(); }
