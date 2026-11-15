// sensor.cpp —— 数据源实现
//   SensorShm    : 从共享文件 /data/data/<pkg>/files/esp_data.bin 读（frida / JS 侧写入）
//   SensorNative : 直接遍历 Cocos Creator 3.x 场景图（偏移由 files/esp_layout.txt 标定）
#include "sensor.h"
#include "shm_abi.h"
#include "config.h"
#include "log.h"
#include "hook.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <map>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>

EspConfig g_cfg;

// ============================================================ 小工具：key=value 布局表
static std::map<std::string, std::string> LoadKeyValues(const char* path) {
    std::map<std::string, std::string> kv;
    FILE* f = fopen(path, "r");
    if (!f) return kv;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = line, v = eq + 1;
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
            size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            s = s.substr(i);
        };
        trim(k); trim(v);
        if (!k.empty()) kv[k] = v;
    }
    fclose(f);
    return kv;
}

static std::string AppFilesDir() {
    // 游戏自身私有目录；stub 运行时 cwd 为 "/"，用 /proc/self/cmdline 取包名
    static std::string dir;
    if (!dir.empty()) return dir;
    char cmd[256] = {0};
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (f) {
        size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
        fclose(f);
        (void)n;
        std::string pkg(cmd);
        size_t colon = pkg.find(':');
        if (colon != std::string::npos) pkg = pkg.substr(0, colon);
        dir = "/data/data/" + pkg + "/files";
    } else {
        dir = "/sdcard";
    }
    return dir;
}

static float GetF(const std::map<std::string, std::string>& kv, const char* k, float def) {
    auto it = kv.find(k);
    return it == kv.end() ? def : (float)atof(it->second.c_str());
}
static long long GetI(const std::map<std::string, std::string>& kv, const char* k, long long def) {
    auto it = kv.find(k);
    return it == kv.end() ? def : strtoll(it->second.c_str(), nullptr, 0);
}

// ============================================================ SensorShm
class SensorShm : public ISensor {
public:
    const char* Name() const override { return "共享内存(外部写入)"; }

    bool Init() override {
        path_ = AppFilesDir() + "/esp_data.bin";
        LOGI("SensorShm path=%s", path_.c_str());
        return true;
    }

    bool Update(ViewInfo& view, std::vector<Entity>& out) override {
        out.clear();
        int fd = open(path_.c_str(), O_RDONLY);
        if (fd < 0) { misses_++; return false; }
        struct stat st{};
        if (fstat(fd, &st) != 0 || (size_t)st.st_size < (size_t)ESP_SHM_HDR_MIN) { close(fd); misses_++; return false; }

        // v4 起改用 read() 全量快照，不再长期持有 mmap 映射：
        // JS 侧 writeDataToFile 是 fopen("wb") 语义，会先把文件截断到 0，
        // 若读端持着旧 mmap，访问越界页会触发 SIGBUS 直接崩进程。
        const size_t fsz = (size_t)st.st_size;
        const size_t need = fsz < (size_t)ESP_SHM_HDR_SZ ? fsz : (size_t)ESP_SHM_HDR_SZ;
        if (buf_.size() < need) buf_.resize(need);
        size_t got = 0;
        while (got < need) {
            ssize_t r = read(fd, buf_.data() + got, need - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
        if (got < need) { misses_++; return false; }

        auto* h = (const ShmHeader*)buf_.data();
        if (h->magic != ESP_SHM_MAGIC) { badVersion_++; return false; }
        // v5 步长 = sizeof(ShmEntity)；v4/v3 尾部少 12 字节（无 camp/aiLevel/liveState），
        // 必须按版本算步长，否则旧文件会被新结构体索引到错位数据。
        int entSz = 0, entVer = 0;
        if (h->version == ESP_SHM_VERSION)      { entSz = ESP_SHM_ENT_SZ_V5; entVer = 5; }
        else if (h->version == 4u || h->version == 3u) { entSz = ESP_SHM_ENT_SZ_V4; entVer = 4; }
        else { badVersion_++; return false; }

        uint64_t s1 = h->seq;
        if (s1 & 1) return false;                        // 写入端正在写
        uint64_t s2 = h->seq;
        if (s1 != s2) return false;

        view.valid = true;
        memcpy(view.viewProj.m, h->view.viewProj, sizeof(float) * 16);
        view.camPos = Vec3(h->view.camPos[0], h->view.camPos[1], h->view.camPos[2]);
        view.fovY = h->view.fovY > 1.f ? h->view.fovY : 60.f;
        view.viewW = h->view.viewW;
        view.viewH = h->view.viewH;

        int n = h->count;
        if (n < 0) n = 0;
        if (n > ESP_SHM_MAX_ENTS) n = ESP_SHM_MAX_ENTS;
        for (int i = 0; i < n; ++i) {
            const size_t eoff = offsetof(ShmHeader, ents) + (size_t)i * (size_t)entSz;
            if (eoff + (size_t)entSz > need) break;      // 文件比声明短：截断而非越界读
            ShmEntity tmp;
            memset(&tmp, 0, sizeof(tmp));
            memcpy(&tmp, buf_.data() + eoff, (size_t)entSz);
            const ShmEntity& s = tmp;
            Entity e;
            e.id = s.id;
            e.head   = Vec3(s.head[0], s.head[1], s.head[2]);
            e.chest  = Vec3(s.chest[0], s.chest[1], s.chest[2]);
            e.pelvis = Vec3(s.pelvis[0], s.pelvis[1], s.pelvis[2]);
            e.feet   = Vec3(s.feet[0], s.feet[1], s.feet[2]);
            e.velocity = Vec3(s.velocity[0], s.velocity[1], s.velocity[2]);
            e.hp = s.hp; e.maxHp = s.maxHp > 0 ? s.maxHp : 100.f;
            e.distance = s.distance > 0 ? s.distance : (e.feet - view.camPos).Length();
            e.visible = (s.flags & ESP_SHM_FLAG_VISIBLE) != 0;
            e.isTeam  = (s.flags & ESP_SHM_FLAG_TEAM) != 0;
            e.isDead  = (s.flags & ESP_SHM_FLAG_DEAD) != 0;
            strncpy(e.name, s.name, sizeof(e.name) - 1);
            e.hasBones = s.hasBones != 0;
            for (int b = 0; b < BoneIdx::Count; ++b)
                e.bones[b] = Vec3(s.bones[b][0], s.bones[b][1], s.bones[b][2]);
            // v4：JS 端已 worldToScreen，屏幕坐标直接透传，渲染层不再依赖 viewProj
            e.screenSpace = (s.flags & ESP_SHM_FLAG_SCREEN) != 0 || s.screenSpace != 0;
            if (e.screenSpace) {
                e.screenHeadX  = s.sHead[0];  e.screenHeadY  = s.sHead[1];
                e.screenChestX = s.sChest[0]; e.screenChestY = s.sChest[1];
                e.screenFeetX  = s.sFeet[0];  e.screenFeetY  = s.sFeet[1];
            }
            // v4：JS 已解算好的提前量/下坠/命中点屏幕坐标
            e.hasLead = (s.flags & ESP_SHM_FLAG_LEAD) != 0;
            if (e.hasLead) {
                e.leadScrX   = s.sLead[0];
                e.leadScrY   = s.sLead[1];
                e.leadMeters = s.leadMeters;
                e.dropMeters = s.dropMeters;
                e.flightTime = s.flightTime;
            }
            int32_t scamp = -1, sai = -1, slive = -1;
            if (entVer >= 5) { scamp = s.camp; sai = s.aiLevel; slive = s.liveState; }
            e.camp = scamp; e.aiLevel = sai; e.liveState = slive;
            e.isAI = (sai > 0);
            e.isKnocked = (slive == 2);
            if (e.screenSpace) ++screenEnts_;
            out.push_back(e);
        }
        hits_++;
        return !out.empty() || true;
    }

    void Dump() override {
        LOGI("[shm] path=%s hits=%llu misses=%llu badver=%llu cache=%u screen=%llu",
             path_.c_str(), (unsigned long long)hits_, (unsigned long long)misses_,
             (unsigned long long)badVersion_, (unsigned)buf_.size(),
             (unsigned long long)screenEnts_);
    }

private:
    std::string path_;
    std::vector<unsigned char> buf_;   // v4：read() 快照缓冲（不再长期持有 mmap）
    uint64_t hits_ = 0, misses_ = 0, badVersion_ = 0;
    uint64_t screenEnts_ = 0;   // v4：累计透传的屏幕坐标实体数（判断 JS 侧是否真在写）
};

// ============================================================ SensorNative
// 说明：Cocos Creator 3.x 的 libcocos.so 已被 strip，无法 dlsym 到 Node/Director 符号，
// 因此这里用「全局指针锚点 + 结构体偏移表」的方式读场景：标定一次，之后稳定可用。
//   files/esp_layout.txt 示例（全部十六进制/十进制均可）：
//     director_ptr      = 0x7a3b1c0010   # 一个存放 Director* 的全局变量地址（绝对地址，标定得到）
//     off_director_scene= 0x18           # Director -> Scene*
//     off_node_name     = 0x30           # Node -> std::string* (c_str 指针在 +0)
//     off_node_children = 0x88           # Node -> std::vector<Node*>{begin,end,cap}
//     off_node_worldpos = 0x140          # Node -> Vec3 worldPosition
//     off_node_pos      = 0x110          # Node -> Vec3 position
//     off_node_active   = 0x9c
//     entity_name_kw    = player,enemy,role,hero   # 节点名含这些词即视为实体
//     off_entity_hp     = 0x0            # 0 表示不读血量
//     off_camera_vp     = 0x0            # 视图投影矩阵所在偏移（0=不使用，回退到 egl 抓取）
class SensorNative : public ISensor {
public:
    const char* Name() const override { return "Cocos 场景图(需标定)"; }

    bool Init() override {
        layoutPath_ = AppFilesDir() + "/esp_layout.txt";
        auto kv = LoadKeyValues(layoutPath_.c_str());
        if (kv.empty()) {
            LOGW("未找到 %s，Native 数据源处于未标定状态", layoutPath_.c_str());
            ready_ = false;
            return false;
        }
        directorPtr_   = (uintptr_t)GetI(kv, "director_ptr", 0);
        offScene_      = (int)GetI(kv, "off_director_scene", 0x18);
        offName_       = (int)GetI(kv, "off_node_name", 0x30);
        offChildren_   = (int)GetI(kv, "off_node_children", 0x88);
        offWorldPos_   = (int)GetI(kv, "off_node_worldpos", 0x140);
        offPos_        = (int)GetI(kv, "off_node_pos", 0x110);
        offActive_     = (int)GetI(kv, "off_node_active", 0x9c);
        kw_            = kv["entity_name_kw"];
        ready_ = directorPtr_ != 0;
        LOGI("layout loaded ready=%d director_ptr=%p children_off=0x%x worldpos_off=0x%x",
             ready_, (void*)directorPtr_, offChildren_, offWorldPos_);
        return ready_;
    }

    bool Update(ViewInfo& view, std::vector<Entity>& out) override {
        out.clear();
        if (!ready_) return false;
        void* director = ReadPtr(directorPtr_);
        if (!director) return false;
        void* scene = ReadPtr((uintptr_t)director + offScene_);
        if (!scene) return false;

        std::vector<void*> stack{scene};
        int visited = 0;
        while (!stack.empty() && visited < 4096) {
            void* node = stack.back(); stack.pop_back();
            ++visited;
            if (!node) continue;

            std::string nm = ReadName((uintptr_t)node + offName_);
            if (MatchKeyword(nm)) {
                Entity e;
                e.id = (uint64_t)(uintptr_t)node;
                Vec3 w = ReadVec3((uintptr_t)node + offWorldPos_);
                e.feet = w;
                e.head = Vec3(w.x, w.y + g_cfg.targetHeightHead / (g_cfg.worldScale > 0 ? g_cfg.worldScale : 1.f), w.z);
                e.chest = Vec3(w.x, w.y + g_cfg.targetHeightChest, w.z);
                strncpy(e.name, nm.c_str(), sizeof(e.name) - 1);
                e.distance = (w - view.camPos).Length();
                out.push_back(e);
            }
            AppendChildren((uintptr_t)node + offChildren_, stack);
        }
        return !out.empty();
    }

    void Dump() override {
        if (!ready_) { LOGW("Native 未标定，dump 跳过"); return; }
        void* director = ReadPtr(directorPtr_);
        void* scene = director ? ReadPtr((uintptr_t)director + offScene_) : nullptr;
        LOGI("dump director=%p scene=%p", director, scene);
        if (!scene) return;
        std::vector<std::pair<void*, int>> stack{{scene, 0}};
        int printed = 0;
        while (!stack.empty() && printed < 400) {
            auto [node, depth] = stack.back(); stack.pop_back();
            if (!node) continue;
            std::string nm = ReadName((uintptr_t)node + offName_);
            Vec3 wp = ReadVec3((uintptr_t)node + offWorldPos_);
            LOGI("%*s[%p] '%s' world=(%.2f, %.2f, %.2f)", depth * 2, "", node,
                 nm.c_str(), wp.x, wp.y, wp.z);
            ++printed;
            std::vector<void*> kids;
            AppendChildren((uintptr_t)node + offChildren_, kids);
            for (void* k : kids) stack.push_back({k, depth + 1});
        }
    }

private:
    static bool Readable(uintptr_t p, size_t n) {
        FILE* f = fopen("/proc/self/maps", "r");
        if (!f) return false;
        char line[512];
        bool ok = false;
        while (fgets(line, sizeof(line), f)) {
            uintptr_t s = 0, e = 0; char perm[8] = {0};
            if (sscanf(line, "%lx-%lx %7s", &s, &e, perm) != 3) continue;
            if (p >= s && p + n <= e) { ok = perm[0] == 'r'; break; }
        }
        fclose(f);
        return ok;
    }
    static void* ReadPtr(uintptr_t a) {
        if (!Readable(a, 8)) return nullptr;
        return *(void**)a;
    }
    static Vec3 ReadVec3(uintptr_t a) {
        if (!Readable(a, 12)) return Vec3();
        float* p = (float*)a;
        return Vec3(p[0], p[1], p[2]);
    }
    static std::string ReadName(uintptr_t a) {
        // Cocos 使用 std::string 存节点名：SSO 时内容在 +16，长串时 +0 为堆指针
        if (!Readable(a, 32)) return {};
        uint8_t* s = (uint8_t*)a;
        uintptr_t cap = *(uintptr_t*)(s + 16);
        if (cap > 4096) return {};
        const char* data;
        if (cap > 0 && Readable((uintptr_t)s, 8)) {
            void* heap = *(void**)s;
            if (heap && Readable((uintptr_t)heap, 64)) data = (const char*)heap;
            else data = (const char*)(s + 16);
        } else {
            data = (const char*)(s + 16);
        }
        if (!Readable((uintptr_t)data, 40)) return {};
        char buf[41]; memcpy(buf, data, 40); buf[40] = 0;
        for (int i = 0; i < 40; ++i) if (buf[i] && (buf[i] < 32 || buf[i] > 126)) return {};
        return std::string(buf);
    }
    void AppendChildren(uintptr_t vecAddr, std::vector<void*>& out) {
        if (!Readable(vecAddr, 24)) return;
        void** begin = *(void***)vecAddr;
        void** end   = *(void***)(vecAddr + 8);
        if (!begin || !end) return;
        int64_t n = (intptr_t)end - (intptr_t)begin;
        if (n <= 0 || n > 4096 * 8) return;
        int cnt = (int)(n / 8);
        if (cnt > 512) cnt = 512;
        if (!Readable((uintptr_t)begin, cnt * 8)) return;
        for (int i = 0; i < cnt; ++i) out.push_back(begin[i]);
    }
    bool MatchKeyword(const std::string& nm) const {
        if (nm.empty() || kw_.empty()) return false;
        std::string low = nm;
        for (auto& c : low) c = (char)tolower(c);
        size_t start = 0;
        while (start <= kw_.size()) {
            size_t comma = kw_.find(',', start);
            std::string k = kw_.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!k.empty() && low.find(k) != std::string::npos) return true;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return false;
    }

    std::string layoutPath_, kw_;
    uintptr_t directorPtr_ = 0;
    int offScene_ = 0x18, offName_ = 0x30, offChildren_ = 0x88, offWorldPos_ = 0x140,
        offPos_ = 0x110, offActive_ = 0x9c;
    bool ready_ = false;
};

// ============================================================ 选择器
ISensor* CreateShmSensor()    { return new SensorShm(); }
ISensor* CreateSceneSensor();                     // sensor_native.cpp
ISensor* CreateNativeSensor() { return CreateSceneSensor(); }

static ISensor* g_sensor = nullptr;
static pthread_mutex_t g_sensorMtx = PTHREAD_MUTEX_INITIALIZER;

static bool ShmFileExists() {
    const std::string p = AppFilesDir() + "/esp_data.bin";
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && (size_t)st.st_size >= sizeof(ShmHeader);
}

ISensor* GetSensor() {
    pthread_mutex_lock(&g_sensorMtx);
    if (!g_sensor) {
        // sensorMode: 0/1 = 场景图直读，2 = 强制外部 shm，3 = 自动优先 JS 数据（无文件回落场景图）
        ISensor* s = nullptr;
        if (g_cfg.sensorMode == 2) {
            s = CreateShmSensor();
        } else if (g_cfg.sensorMode == 3) {
            if (ShmFileExists()) {
                s = CreateShmSensor();
                LOGI("自动数据源：检测到 esp_data.bin → 使用 JS 数据");
            } else {
                s = CreateNativeSensor();
                LOGI("自动数据源：无 esp_data.bin → 回落场景遍历");
            }
        } else {
            s = CreateNativeSensor();
        }
        if (s && !s->Init()) {
            LOGW("数据源 %s 初始化失败", s->Name());
        } else if (s) {
            LOGI("数据源就绪：%s", s->Name());
        }
        g_sensor = s;
    }
    pthread_mutex_unlock(&g_sensorMtx);
    return g_sensor;
}

void ResetSensor() {
    pthread_mutex_lock(&g_sensorMtx);
    delete g_sensor;
    g_sensor = nullptr;
    pthread_mutex_unlock(&g_sensorMtx);
}

bool ProbeShmOnce() {
    const std::string p = AppFilesDir() + "/esp_data.bin";
    int fd = open(p.c_str(), O_RDONLY);
    if (fd < 0) return false;
    ShmHeader h{};
    const ssize_t n = read(fd, &h, sizeof(h));
    close(fd);
    if (n < (ssize_t)offsetof(ShmHeader, ents)) return false;
    if (h.magic != ESP_SHM_MAGIC) return false;
    if (h.version != ESP_SHM_VERSION && h.version != 3u) return false;
    return h.count > 0;
}
