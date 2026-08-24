#include "broker/broker.h"
#include <iostream>
#include <cstring>
#include <sstream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <errno.h>
#include <cmath>
#include <endian.h>
#include <gmpxx.h>
#include <fstream>
#include <functional>
#include "crypto/Paillier.h"

#define MAX_EVENTS 10
#define TTL_SECONDS 15
#define CLEANUP_INTERVAL_SEC 10
#define EPOLL_TIMEOUT_MS 3000

// Helper: hex string to mpz_class
mpz_class hexToMpz(const std::string& hex) {
    mpz_class val;
    mpz_set_str(val.get_mpz_t(), hex.c_str(), 16);
    return val;
}

// Helper: mpz_class to hex string
std::string mpzToHex(const mpz_class& val) {
    char* str = mpz_get_str(nullptr, 16, val.get_mpz_t());
    std::string result(str);
    free(str);
    return result;
}

// ============================================================
// Construction / Destruction
// ============================================================

DnsMulticastBroker::DnsMulticastBroker(const BrokerConfig& config)
    : server_fd(-1), config_(config)
{
    has_parent_ = !config_.parent_addr.empty();
    
    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0) {
        std::cerr << "Socket creation failed!" << std::endl;
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(config_.listen_port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed on port " << config_.listen_port << "!" << std::endl;
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (has_parent_) {
        size_t colon = config_.parent_addr.find(':');
        std::string ip = config_.parent_addr.substr(0, colon);
        uint16_t port = static_cast<uint16_t>(std::stoi(config_.parent_addr.substr(colon + 1)));
        
        parent_addr_.sin_family = AF_INET;
        parent_addr_.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &parent_addr_.sin_addr);
    }

    my_region_.min_lat = my_region_.max_lat = config_.lat;
    my_region_.min_lon = my_region_.max_lon = config_.lon;

    // Phase 3: Initialize Crypto
    initCrypto();

    std::stringstream ss;
    ss << "[Broker " << config_.broker_id << "] Listening on port " << config_.listen_port
       << " (brake_limit=" << config_.brake_limit << ")";
    if (has_parent_) {
        ss << " (parent: " << config_.parent_addr << ")";
    } else {
        ss << " (ROOT)";
    }
    if (he_enabled_) {
        ss << " (HE Enabled)";
    }
    logger.pushLog(ss.str());

    // 运行模式声明：一行就能确认这次跑的到底是不是加密模式、brake 参数是多少。
    // 没有这一行，「加密实验静默退回明文」在输出里是看不出来的（CLAUDE.md I5）。
    const char* scope_str =
        (config_.brake_scope == BrakeScope::Upward) ? "upward" :
        (config_.brake_scope == BrakeScope::Local)  ? "local"  : "both";
    std::stringstream mode;
    mode << "[Broker " << config_.broker_id << "] MODE"
         << " he_enabled=" << (he_enabled_ ? 1 : 0)
         << " has_parent=" << (has_parent_ ? 1 : 0)
         << " brake_limit=" << config_.brake_limit
         << " brake_window=" << config_.brake_window_sec << "s"
         << " brake_scope=" << scope_str;
    logger.pushLog(mode.str());

    if (he_enabled_) {
        // 只打印公钥模数 n 的位长和指纹，不打印任何密钥材料本身。
        // 位长单独无法区分两把不同的 2048 位密钥，所以额外给一个指纹，
        // 用来确认 broker 和客户端加载的是同一把。
        std::string n_hex = mpzToHex(he_n_);
        std::stringstream fp;
        fp << std::hex << std::hash<std::string>{}(n_hex);
        std::stringstream key;
        key << "[Broker " << config_.broker_id << "] HE_KEY"
            << " source=" << he_key_source_
            << " n_bits=" << mpz_sizeinbase(he_n_.get_mpz_t(), 2)
            << " n_fp=" << fp.str();
        logger.pushLog(key.str());
    }
}

DnsMulticastBroker::~DnsMulticastBroker() {
    close(server_fd);
}

// ============================================================
// Phase 3: Crypto Initialization
// ============================================================
void DnsMulticastBroker::initCrypto() {
    std::string key_file = "/tmp/dnspp_heps.key";
    std::string full_key_file = "/tmp/dnspp_heps_full.key";
    
    if (!has_parent_) {
        Paillier p;
        p.keyGen(2048);
        he_n_ = p.getN();
        he_mu_ = p.getMu();
        he_n_sq_ = he_n_ * he_n_;
        he_enabled_ = true;
        he_key_source_ = "generated(root)";

        // Save public key for leaf brokers
        std::ofstream ofs(key_file);
        ofs << mpzToHex(he_n_) << "\n";
        ofs << mpzToHex(he_mu_) << "\n";
        ofs.close();

        // Save full state for clients (acting as HEPS proxy for blinding)
        std::ofstream ofs_full(full_key_file);
        ofs_full << mpzToHex(p.getN()) << "\n";
        ofs_full << mpzToHex(p.getMu()) << "\n";
        ofs_full << mpzToHex(p.getLambda()) << "\n";
        ofs_full << mpzToHex(p.getEM()) << "\n";
        ofs_full << mpzToHex(p.getDM()) << "\n";
        ofs_full << mpzToHex(p.getRM()) << "\n";
        ofs_full.close();
    } else {
        // Leaf Broker: Read public key from file
        std::ifstream ifs(key_file);
        std::string n_hex, mu_hex;
        if (std::getline(ifs, n_hex) && std::getline(ifs, mu_hex)) {
            he_n_ = hexToMpz(n_hex);
            he_mu_ = hexToMpz(mu_hex);
            he_n_sq_ = he_n_ * he_n_;
            he_enabled_ = true;
            he_key_source_ = key_file;
        } else {
            std::cerr << "Warning: Could not read HEPS key file. Running in plaintext mode." << std::endl;
            he_enabled_ = false;
        }
    }
}

bool DnsMulticastBroker::executeMatch(const std::string& bval_n_hex, 
                                      const std::string& bval_m1_hex, 
                                      const std::string& bval_m2_hex) const {
    // 在函数入口计数：即使因为参数为空提前返回，这次调用也确实发生过。
    stat_match_calls++;
    if (bval_n_hex.empty() || bval_m1_hex.empty() || bval_m2_hex.empty()) return false;

    mpz_class bval_n = hexToMpz(bval_n_hex);
    mpz_class bval_m1 = hexToMpz(bval_m1_hex);
    mpz_class bval_m2 = hexToMpz(bval_m2_hex);

    // 1. Check x >= v
    mpz_class y1 = (bval_n * bval_m1) % he_n_sq_;
    mpz_class L_y1 = (y1 - 1) / he_n_;
    mpz_class diff1 = (L_y1 * he_mu_) % he_n_;

    if (diff1 >= he_n_ / 2) return false; // x < v

    // 2. Check x < v+1
    mpz_class y2 = (bval_n * bval_m2) % he_n_sq_;
    mpz_class L_y2 = (y2 - 1) / he_n_;
    mpz_class diff2 = (L_y2 * he_mu_) % he_n_;

    if (diff2 <= he_n_ / 2) return false; // x >= v+1

    stat_match_hits++;
    return true; // Match!
}

// ============================================================
// Helpers
// ============================================================

std::string DnsMulticastBroker::clientAddrStr(const struct sockaddr_in& addr) const {
    std::stringstream ss;
    ss << inet_ntoa(addr.sin_addr) << ":" << ntohs(addr.sin_port);
    return ss.str();
}

int DnsMulticastBroker::getQuadrant(float lat, float lon) const {
    if (lat >= 0.0f)
        return (lon >= 0.0f) ? 0 : 1;
    else
        return (lon >= 0.0f) ? 2 : 3;
}

bool DnsMulticastBroker::brakeAllows(BrakeDir dir, const std::string& service, float lat, float lon) {
    int q = getQuadrant(lat, lon);
    auto& windows = (dir == BrakeDir::Up) ? brake_count_up_ : brake_count_local_;
    auto& queue = windows[service][q];
    time_t now = time(nullptr);

    while (!queue.empty() && (now - queue.front() > config_.brake_window_sec)) {
        queue.pop();
    }

    if (static_cast<int>(queue.size()) >= config_.brake_limit) {
        // 在这里按方向计数：调用点漏写计数会静默失真（T1 的教训）。
        if (dir == BrakeDir::Up) stat_braked_up++;
        else                     stat_braked_local++;
        return false;
    }

    queue.push(now);
    return true;
}

void DnsMulticastBroker::updateMyRegion() {
    my_region_.min_lat = my_region_.max_lat = config_.lat;
    my_region_.min_lon = my_region_.max_lon = config_.lon;
    for (const auto& [id, child] : children_) {
        my_region_ = Region::merge(my_region_, child.region);
    }
}

// ============================================================
// SUBSCRIBE
// ============================================================
void DnsMulticastBroker::handleSubscribe(const TlvMessage& msg, const struct sockaddr_in& client) {
    stat_sub_received++;   // 含 FROM_CHILD，也含下面因缺字段而被丢弃的
    auto name = msg.getServiceName();
    if (!name) return;

    auto coords = msg.getCoordinates();
    float lat = coords ? coords->first  : 0.0f;
    float lon = coords ? coords->second : 0.0f;

    auto flags_opt = msg.getFlags();
    uint32_t flags = flags_opt ? *flags_opt : 0;
    bool from_child = (flags & MsgFlags::FROM_CHILD);

    if (from_child) {
        std::string child_id = findChildByAddr(client);
        if (child_id.empty()) return;

        child_active_[*name].insert(child_id);

        if (has_parent_ && ot_parent_.find(*name) == ot_parent_.end()) {
            TlvMessageBuilder fwd(MsgType::SUBSCRIBE);
            fwd.addServiceName(*name);
            fwd.addCoordinates(config_.lat, config_.lon);
            fwd.addFlags(MsgFlags::FROM_CHILD);
            auto pkt = fwd.build();
            sendto(server_fd, pkt.data(), pkt.size(), 0, (const struct sockaddr*)&parent_addr_, sizeof(parent_addr_));
            ot_parent_.insert(*name);
        }
        return;
    }

    GeoClient gc;
    gc.addr = client;
    gc.lat  = lat;
    gc.lon  = lon;
    gc.cached_closest_dist = std::numeric_limits<double>::max();

    // Phase 3: Use blinded values as key if available
    auto bval_m1 = msg.getBlindedValue();
    auto bval_m2 = msg.getBlindedValueHi();
    // 使用 hash(name) 模拟 Cover 协议的分组效果
    std::string sub_key = (bval_m1 && he_enabled_) ? hashServiceName(*name) : *name;

    if (bval_m1 && bval_m2) {
        gc.blinded_m1 = *bval_m1;
        gc.blinded_m2 = *bval_m2;
    }

    subscribers[sub_key].push_back(gc);
    last_active[sub_key] = time(nullptr);

    Region old_region = my_region_;
    my_region_.min_lat = std::min(my_region_.min_lat, (double)lat);
    my_region_.max_lat = std::max(my_region_.max_lat, (double)lat);
    my_region_.min_lon = std::min(my_region_.min_lon, (double)lon);
    my_region_.max_lon = std::max(my_region_.max_lon, (double)lon);
    
    if (has_parent_ && 
        (my_region_.min_lat != old_region.min_lat || my_region_.max_lat != old_region.max_lat ||
         my_region_.min_lon != old_region.min_lon || my_region_.max_lon != old_region.max_lon)) {
        sendRegionUpdateToParent();
    }

    if (flags & MsgFlags::QUERY_MODE) {
        auto it = pub_cache.find(sub_key);
        if (it != pub_cache.end() && !it->second.empty()) {
            const CachedPub* best = nullptr;
            double best_dist = std::numeric_limits<double>::max();
            for (const auto& pub : it->second) {
                double d = geoDistance(lat, lon, pub.lat, pub.lon);
                if (d < best_dist) { best_dist = d; best = &pub; }
            }
            if (best) {
                sendto(server_fd, best->raw_message.data(), best->raw_message.size(), 0,
                       (const struct sockaddr*)&client, sizeof(client));
                subscribers[sub_key].back().cached_closest_dist = best_dist;
            }
        }
    }

    if (has_parent_ && ot_parent_.find(sub_key) == ot_parent_.end()) {
        TlvMessageBuilder fwd(MsgType::SUBSCRIBE);
        fwd.addServiceName(sub_key);
        fwd.addCoordinates(config_.lat, config_.lon);
        fwd.addFlags(MsgFlags::FROM_CHILD);
        auto pkt = fwd.build();
        sendto(server_fd, pkt.data(), pkt.size(), 0, (const struct sockaddr*)&parent_addr_, sizeof(parent_addr_));
        ot_parent_.insert(sub_key);
    }
}

// ============================================================
// PUBLISH
// ============================================================
void DnsMulticastBroker::handlePublish(const TlvMessage& msg, const struct sockaddr_in& client) {
    stat_pub_received++;   // 含来自 parent/child 的，也含下面因缺字段而被丢弃的
    auto name = msg.getServiceName();
    if (!name) return;
    
    auto coords = msg.getCoordinates();
    float pub_lat = coords ? coords->first  : 0.0f;
    float pub_lon = coords ? coords->second : 0.0f;

    auto flags_opt = msg.getFlags();
    uint32_t flags = flags_opt ? *flags_opt : 0;
    bool from_child = (flags & MsgFlags::FROM_CHILD);
    bool from_parent = (flags & MsgFlags::FROM_PARENT);
    
    std::string from_id = from_child ? findChildByAddr(client) : (from_parent ? "parent" : clientAddrStr(client));

    CachedPub cp;
    cp.lat = pub_lat;
    cp.lon = pub_lon;
    cp.raw_message.assign(msg.getRawData(), msg.getRawData() + msg.getRawSize());
    
    // Phase 3: Determine pub_key
    auto bval_n_opt = msg.getBlindedValue();
    std::string pub_key = (bval_n_opt && he_enabled_) ? hashServiceName(*name) : *name;
    
    pub_cache[pub_key].push_back(std::move(cp));

    constexpr size_t MAX_PUB_CACHE = 10;
    if (pub_cache[pub_key].size() > MAX_PUB_CACHE) {
        pub_cache[pub_key].erase(pub_cache[pub_key].begin());
    }

    // 2. 向上传播（brake_scope 为 upward 或 both 时受 brake 限流）
    bool brake_upward = (config_.brake_scope == BrakeScope::Upward ||
                         config_.brake_scope == BrakeScope::Both);
    if (has_parent_ && !from_parent) {
        if (!brake_upward || brakeAllows(BrakeDir::Up, pub_key, pub_lat, pub_lon)) {
            TlvMessageBuilder fwd(MsgType::PUBLISH);
            fwd.addServiceName(pub_key);
            fwd.addCoordinates(pub_lat, pub_lon);
            if (msg.getPayload() && msg.getPayloadSize() > 0) {
                fwd.setPayload(msg.getPayload(), msg.getPayloadSize());
            }
            if (bval_n_opt) fwd.addBlindedValue(*bval_n_opt);
            fwd.addFlags(MsgFlags::FROM_CHILD);
            auto pkt = fwd.build();
            sendto(server_fd, pkt.data(), pkt.size(), 0, (const struct sockaddr*)&parent_addr_, sizeof(parent_addr_));
            stat_forward_up++;
        }
        // 上行 brake 拒绝的情况已在 brakeAllows(Up) 内部计入 stat_braked_up
    }

    // 3. 向下传播
    auto child_it = child_active_.find(pub_key);
    if (child_it != child_active_.end()) {
        for (const auto& child_id : child_it->second) {
            if (child_id == from_id) continue;
            auto child_node_it = children_.find(child_id);
            if (child_node_it == children_.end()) continue;
            
            ChildBroker& child = child_node_it->second;
            int q = child.region.quadrantOf(pub_lat, pub_lon);
            auto centers = child.region.quadrantCenters();
            double dist = geoDistance(pub_lat, pub_lon, centers[q].first, centers[q].second);
            
            if (dist < child.closest_quad[q]) {
                child.closest_quad[q] = dist;
                TlvMessageBuilder fwd(MsgType::PUBLISH);
                fwd.addServiceName(pub_key);
                fwd.addCoordinates(pub_lat, pub_lon);
                if (msg.getPayload() && msg.getPayloadSize() > 0) {
                    fwd.setPayload(msg.getPayload(), msg.getPayloadSize());
                }
                if (bval_n_opt) fwd.addBlindedValue(*bval_n_opt);
                fwd.addFlags(MsgFlags::FROM_PARENT);
                auto pkt = fwd.build();
                sendto(server_fd, pkt.data(), pkt.size(), 0, (const struct sockaddr*)&child.addr, sizeof(child.addr));
                stat_forward_down++;
            }
        }
    }

    // 4. 本地投递
    auto it = subscribers.find(pub_key);
    if (it == subscribers.end() || it->second.empty()) return;

    // 本地投递 brake 门（brake_scope 为 local 或 both 时生效）。
    // 位置很关键：必须在「pub_cache 已缓存之后」——query_mode 依赖那份缓存，
    // 不能因为被 brake 拦截就不缓存（缓存在本函数前面已完成）。也必须在
    // 「确认存在订阅者之后」——没有订阅者时本地不投递，也就无所谓限流，这与
    // Phase 1 (commit b79ea7b) 的原始语义一致。
    bool brake_local = (config_.brake_scope == BrakeScope::Local ||
                        config_.brake_scope == BrakeScope::Both);
    if (brake_local && !brakeAllows(BrakeDir::Local, pub_key, pub_lat, pub_lon)) {
        // 被本地 brake 拦下：不投递（stat_braked_local 已在 brakeAllows 内自增）
        return;
    }

    int delivered = 0;
    const uint8_t* raw = msg.getRawData();
    size_t raw_len = msg.getRawSize();

    if (he_enabled_ && bval_n_opt) {
        // Phase 3: 加密匹配模式 (O(1) 查找 + 单次 Match)
        const auto& gc0 = it->second.front();
        if (gc0.blinded_m1.empty()) return;

        // 只对组内第一个订阅者做一次 Match
        if (!executeMatch(*bval_n_opt, gc0.blinded_m1, gc0.blinded_m2)) {
            return; // 不匹配，丢弃
        }

        // 匹配成功，投递给该组的所有订阅者
        for (auto& gc : it->second) {
            double dist = geoDistance(pub_lat, pub_lon, gc.lat, gc.lon);
            if (dist < gc.cached_closest_dist) {
                sendto(server_fd, raw, raw_len, 0, (const struct sockaddr*)&gc.addr, sizeof(gc.addr));
                gc.cached_closest_dist = dist;
                delivered++;
                stat_delivered_local++;
            }
        }
    } else {
        // 明文模式
        for (auto& gc : it->second) {
            double dist = geoDistance(pub_lat, pub_lon, gc.lat, gc.lon);
            if (dist < gc.cached_closest_dist) {
                sendto(server_fd, raw, raw_len, 0, (const struct sockaddr*)&gc.addr, sizeof(gc.addr));
                gc.cached_closest_dist = dist;
                delivered++;
                stat_delivered_local++;
            }
        }
    }
}

// ============================================================
// Other Handlers (Unchanged from Phase 2)
// ============================================================

void DnsMulticastBroker::handleHeartbeat(const TlvMessage& msg, const struct sockaddr_in& client) {
    (void)client;
    auto name = msg.getServiceName();
    if (!name) return;
    last_active[*name] = time(nullptr);
}

void DnsMulticastBroker::handleHello(const TlvMessage& msg, const struct sockaddr_in& child_addr) {
    auto child_id = msg.getServiceName();
    auto coords = msg.getCoordinates();
    if (!child_id || !coords) return;

    float lat = coords->first;
    float lon = coords->second;

    ChildBroker& child = children_[*child_id];
    child.id = *child_id;
    child.addr = child_addr;
    child.lat = lat;
    child.lon = lon;
    child.region.min_lat = child.region.max_lat = lat;
    child.region.min_lon = child.region.max_lon = lon;

    updateMyRegion();

    TlvMessageBuilder ack(MsgType::HELLO_ACK);
    ack.addServiceName(config_.broker_id);
    auto pkt = ack.build();
    sendto(server_fd, pkt.data(), pkt.size(), 0, (const struct sockaddr*)&child_addr, sizeof(child_addr));
}

std::string DnsMulticastBroker::findChildByAddr(const struct sockaddr_in& addr) const {
    for (const auto& [id, child] : children_) {
        if (child.addr.sin_port == addr.sin_port && child.addr.sin_addr.s_addr == addr.sin_addr.s_addr) {
            return id;
        }
    }
    return "";
}

std::string DnsMulticastBroker::hashServiceName(const std::string& name) const {
    std::hash<std::string> h;
    return std::to_string(h(name));
}

void DnsMulticastBroker::sendRegionUpdateToParent() {
    if (!has_parent_) return;
    TlvMessageBuilder msg(MsgType::REGION_UPDATE);
    msg.addServiceName(config_.broker_id);
    msg.addRegion(my_region_);
    auto pkt = msg.build();
    sendto(server_fd, pkt.data(), pkt.size(), 0, (const struct sockaddr*)&parent_addr_, sizeof(parent_addr_));
}

void DnsMulticastBroker::handleRegionUpdate(const TlvMessage& msg, const struct sockaddr_in& child_addr) {
    (void)child_addr;
    auto child_id = msg.getServiceName();
    auto new_region = msg.getRegion();
    if (!child_id || !new_region) return;

    auto child_it = children_.find(*child_id);
    if (child_it == children_.end()) return;

    child_it->second.region = *new_region;
    Region old_region = my_region_;
    updateMyRegion();

    if (has_parent_ &&
        (my_region_.min_lat != old_region.min_lat || my_region_.max_lat != old_region.max_lat ||
         my_region_.min_lon != old_region.min_lon || my_region_.max_lon != old_region.max_lon)) {
        sendRegionUpdateToParent();
    }
}

void DnsMulticastBroker::handleStatsRequest(const struct sockaddr_in& client) {
    // 快照量：每次上报时从实时状态重新取，不做增量维护，避免与真实状态漂移。
    // stat_sub_groups 是 subscribers 表里 key 的个数 —— 如果 N 个订阅同一个名字的
    // 订阅者真的合并成了一组，它应当是 1 而不是 N。
    stat_sub_groups = subscribers.size();
    stat_he_mode = he_enabled_ ? 1 : 0;

    TlvMessageBuilder resp(MsgType::STATS_RESPONSE);

    // --- 旧的 32 字节 STATS_DATA (0x0006)，保持不变以免旧解析代码失效 ---
    uint8_t stats_buf[32];
    uint64_t net_up = htobe64(stat_forward_up);
    uint64_t net_down = htobe64(stat_forward_down);
    uint64_t net_local = htobe64(stat_delivered_local);
    // 合计值保留在旧字段里（up + local），旧解析代码看到的语义不变
    uint64_t stat_braked_total = stat_braked_up + stat_braked_local;
    uint64_t net_braked = htobe64(stat_braked_total);
    
    std::memcpy(stats_buf, &net_up, 8);
    std::memcpy(stats_buf + 8, &net_down, 8);
    std::memcpy(stats_buf + 16, &net_local, 8);
    std::memcpy(stats_buf + 24, &net_braked, 8);
    
    resp.addTlv(TlvType::STATS_DATA, stats_buf, 32);

    // --- 新的 96 字节 STATS_DATA_EXT (0x0007)，12 个 big-endian uint64 ---
    // 顺序必须与 README「TLV Field Types」表一致。索引 0-9 与 T1 完全相同（其中
    // 索引 3 的 braked 是合计 = braked_up + braked_local，向后兼容 T1 的解析代码），
    // T3 在末尾追加索引 10/11 给出上行/本地的拆分：
    // forward_up, forward_down, delivered_local, braked,
    // match_calls, match_hits, pub_received, sub_received, sub_groups, he_mode,
    // braked_up, braked_local
    const uint64_t ext_values[12] = {
        stat_forward_up,
        stat_forward_down,
        stat_delivered_local,
        stat_braked_total,
        stat_match_calls,
        stat_match_hits,
        stat_pub_received,
        stat_sub_received,
        stat_sub_groups,
        stat_he_mode,
        stat_braked_up,
        stat_braked_local
    };
    uint8_t ext_buf[96];
    for (size_t i = 0; i < 12; i++) {
        uint64_t be = htobe64(ext_values[i]);
        std::memcpy(ext_buf + i * 8, &be, 8);
    }
    resp.addTlv(TlvType::STATS_DATA_EXT, ext_buf, 96);

    auto pkt = resp.build();
    sendto(server_fd, pkt.data(), pkt.size(), 0, (const struct sockaddr*)&client, sizeof(client));
}

void DnsMulticastBroker::cleanupExpired() {
    time_t now = time(nullptr);
    for (auto it = subscribers.begin(); it != subscribers.end(); ) {
        const auto& name = it->first;
        if (now - last_active[name] > TTL_SECONDS) {
            last_active.erase(name);
            pub_cache.erase(name);
            brake_count_up_.erase(name);
            brake_count_local_.erase(name);
            it = subscribers.erase(it);
        } else {
            ++it;
        }
    }
}

void DnsMulticastBroker::start() {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) return;

    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) return;

    if (has_parent_) {
        TlvMessageBuilder hello(MsgType::HELLO);
        hello.addServiceName(config_.broker_id);
        hello.addCoordinates(config_.lat, config_.lon);
        auto pkt = hello.build();
        sendto(server_fd, pkt.data(), pkt.size(), 0, (const struct sockaddr*)&parent_addr_, sizeof(parent_addr_));
    }

    struct epoll_event events[MAX_EVENTS];
    uint8_t buffer[4096]; // Increased buffer size for large blinded values
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    time_t last_cleanup = time(nullptr);

    while (true) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd != server_fd) continue;

            int bytes = recvfrom(server_fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &client_len);
            if (bytes <= 0) continue;

            TlvMessage msg(buffer, static_cast<size_t>(bytes));
            if (!msg.isValid()) continue;

            switch (msg.getMsgType()) {
                case MsgType::SUBSCRIBE: handleSubscribe(msg, client_addr); break;
                case MsgType::REGION_UPDATE: handleRegionUpdate(msg, client_addr); break;
                case MsgType::PUBLISH: handlePublish(msg, client_addr); break;
                case MsgType::HEARTBEAT: handleHeartbeat(msg, client_addr); break;
                case MsgType::HELLO: handleHello(msg, client_addr); break;
                case MsgType::HELLO_ACK: break;
                case MsgType::STATS_REQUEST: handleStatsRequest(client_addr); break;
                default: break;
            }
        }

        time_t now = time(nullptr);
        if (now - last_cleanup > CLEANUP_INTERVAL_SEC) {
            cleanupExpired();
            last_cleanup = now;
        }
    }
    close(epoll_fd);
}