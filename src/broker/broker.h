#ifndef BROKER_H
#define BROKER_H

#include <unordered_set>
#include <queue>
#include <netinet/in.h>
#include <unordered_map>
#include <vector>
#include <ctime>
#include <limits>
#include <optional>
#include <string>
#include <array>
#include <gmpxx.h>

#include "protocol/TlvMessage.h"
#include "logger/logger.h"
#include "utils/geo.h"

struct GeoClient {
    struct sockaddr_in addr;
    float lat;
    float lon;
    double cached_closest_dist = std::numeric_limits<double>::max();
    std::string blinded_m1; // Phase 3: bval_m(E(-v))
    std::string blinded_m2; // Phase 3: bval_m(E(-(v+1)))
};

struct CachedPub {
    float lat;
    float lon;
    std::vector<uint8_t> raw_message;
};

struct ChildBroker {
    std::string id;
    struct sockaddr_in addr;
    float lat;
    float lon;
    Region region;
    bool active = false;
    std::array<double, 4> closest_quad = {
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max()
    };
};

// brake 作用的传播方向。见 docs/audit_2026-08.md Q1：Phase 2 重构把 brake 从
// 门控本地投递错误地移成了只门控上行传播，导致单 broker 下 brake 完全失效。
// 这个枚举把「作用于哪些方向」变成显式配置。
enum class BrakeScope {
    Upward,  // 只限流向父 broker 的上行传播（Phase 2 回归后的行为）
    Local,   // 只限流本地投递
    Both     // 两者都限流（默认；最接近论文 Algorithm 1 的通用限流语义）
};

struct BrokerConfig {
    std::string broker_id;
    uint16_t listen_port;
    std::string parent_addr;
    float lat;
    float lon;
    int brake_limit;
    time_t brake_window_sec;
    BrakeScope brake_scope = BrakeScope::Both;
    // require_he=true 时 leaf broker 读不到密钥必须 fail-fast，禁止退化为明文。
    // 默认 false 保留旧行为（打印 WARNING 后退化），是为了不破坏现有测试脚本；
    // 学术实验请显式 require_he=true 以防止「加密实验静默变成明文实验」。
    bool require_he = false;
};

class DnsMulticastBroker {
public:
    explicit DnsMulticastBroker(const BrokerConfig& config);
    ~DnsMulticastBroker();
    void start();

private:
    int server_fd;
    BrokerConfig config_;
    
    Region my_region_;
    bool has_parent_ = false;
    struct sockaddr_in parent_addr_;
    std::unordered_map<std::string, ChildBroker> children_;

    Logger logger;

    // ---------- 路由表 ----------
    std::unordered_map<std::string, std::vector<GeoClient>> subscribers;
    std::unordered_map<std::string, std::vector<CachedPub>> pub_cache;
    std::unordered_map<std::string, time_t> last_active;
    // 上行与本地各自维护独立的按象限滑动窗口。共用一个窗口会让一条 publication
    // 在 brake_scope=both 时被两条路径各消耗一个配额（同一条消息被计两次），
    // 所以必须分开。
    std::unordered_map<std::string, std::unordered_map<int, std::queue<time_t>>> brake_count_up_;
    std::unordered_map<std::string, std::unordered_map<int, std::queue<time_t>>> brake_count_local_;
    std::unordered_map<std::string, std::unordered_set<std::string>> child_active_;
    std::unordered_set<std::string> ot_parent_;

    // ---------- 统计计数器 ----------
    uint64_t stat_forward_up = 0;
    uint64_t stat_forward_down = 0;
    uint64_t stat_delivered_local = 0;
    // T3：brake 拆成上行/本地两个方向独立计数。旧的合计值（= up + local）
    // 仍在 STATS_DATA(0x0006) 与 STATS_DATA_EXT 的 braked 槽里上报，向后兼容。
    uint64_t stat_braked_up = 0;
    uint64_t stat_braked_local = 0;

    // 可观测性计数器（CLAUDE.md I5：加密模式必须可观测）。
    // match_calls/match_hits 在 executeMatch() 内部自增，而 executeMatch() 是 const，
    // 所以这两个必须是 mutable —— 放在调用点计数会在新增调用点时静默漏计。
    mutable uint64_t stat_match_calls = 0;
    mutable uint64_t stat_match_hits = 0;
    uint64_t stat_pub_received = 0;
    uint64_t stat_sub_received = 0;
    // 下面两个不是累加计数器，而是快照量：在 handleStatsRequest() 里从实时状态
    // 重新取值，避免与真实状态漂移。
    uint64_t stat_sub_groups = 0;
    uint64_t stat_he_mode = 0;

    // ---------- Phase 3: Encrypted Matching ----------
    mpz_class he_n_;
    mpz_class he_mu_;
    mpz_class he_n_sq_;
    bool he_enabled_ = false;
    std::string he_key_source_;   // 启动日志用：密钥是自己生成的还是从文件加载的

    bool executeMatch(const std::string& bval_n_hex, 
                      const std::string& bval_m1_hex, 
                      const std::string& bval_m2_hex) const;
    void initCrypto();

    // ---------- 辅助函数 ----------
    std::string clientAddrStr(const struct sockaddr_in& addr) const;
    int getQuadrant(float lat, float lon) const;
    // brake 的方向：决定用哪个滑动窗口、给哪个计数器计数。
    enum class BrakeDir { Up, Local };
    bool brakeAllows(BrakeDir dir, const std::string& service, float lat, float lon);
    void updateMyRegion();
    std::string findChildByAddr(const struct sockaddr_in& addr) const;
    std::string hashServiceName(const std::string& name) const;

    // ---------- 消息处理器 ----------
    void handleSubscribe(const TlvMessage& msg, const struct sockaddr_in& client);
    void handlePublish(const TlvMessage& msg, const struct sockaddr_in& client);
    void handleHeartbeat(const TlvMessage& msg, const struct sockaddr_in& client);
    void handleHello(const TlvMessage& msg, const struct sockaddr_in& child_addr);
    void handleRegionUpdate(const TlvMessage& msg, const struct sockaddr_in& child_addr);
    void sendRegionUpdateToParent();
    void handleStatsRequest(const struct sockaddr_in& client);
    void cleanupExpired();
};

#endif