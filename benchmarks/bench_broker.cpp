// benchmarks/bench_broker.cpp
//
// Automated benchmark for DNS++ Phase 1/3
//
// Usage:
//   ./bench_broker <broker_ip> <broker_port> <num_pubs> <num_subs> <brake_limit> <num_trials> [seed] [encrypted=0/1]
//
// Output:
//   stdout: CSV (per-subscriber detail, one row per subscriber per trial)
//   stderr: Summary (per-trial aggregate)

#include "protocol/TlvMessage.h"
#include "utils/geo.h"
#include "crypto/Heps.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <thread>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <endian.h>

struct Publisher {
    int id;
    float lat;
    float lon;
};

// STATS_DATA_EXT (TLV 0x0007): 10 x uint64_t big-endian.
// 顺序与 broker 的 handleStatsRequest() 一致。
static const char* kStatNames[10] = {
    "forward_up", "forward_down", "delivered_local", "braked",
    "match_calls", "match_hits", "pub_received", "sub_received",
    "sub_groups", "he_mode"
};

// 向 broker 请求扩展统计。返回 false 表示没拿到（broker 没响应，或者是一个
// 还不认识 0x0007 的旧 broker）—— 调用方必须把这种情况显式报出来，
// 不能当成「计数器全 0」。
bool queryStatsExt(const struct sockaddr_in& broker_addr, uint64_t out[10]) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    TlvMessageBuilder req(MsgType::STATS_REQUEST);
    auto pkt = req.build();
    sendto(fd, pkt.data(), pkt.size(), 0,
           (const struct sockaddr*)&broker_addr, sizeof(broker_addr));

    struct timeval tv = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[2048];
    int n = recv(fd, buf, sizeof(buf), 0);
    close(fd);
    if (n <= 0) return false;

    TlvMessage msg(buf, static_cast<size_t>(n));
    if (!msg.isValid() || msg.getMsgType() != MsgType::STATS_RESPONSE) return false;

    uint16_t len = 0;
    const uint8_t* v = msg.findTlv(TlvType::STATS_DATA_EXT, &len);
    if (!v || len < 80) return false;

    for (int i = 0; i < 10; i++) {
        uint64_t be;
        std::memcpy(&be, v + i * 8, 8);
        out[i] = be64toh(be);
    }
    return true;
}

struct Subscriber {
    int id;
    float lat;
    float lon;
    int fd = -1;

    int optimal_pub_id   = -1;
    double optimal_dist  = 0.0;

    struct ReceivedPub {
        int pub_id;
        double latency_ms;
    };
    std::vector<ReceivedPub> received;
};

int main(int argc, char* argv[]) {
    if (argc < 7) {
        std::cerr << "Usage: " << argv[0]
                  << " <broker_ip> <broker_port> <num_pubs> <num_subs>"
                  << " <brake_limit> <num_trials> [seed] [encrypted=0/1]" << std::endl;
        return 1;
    }

    std::string broker_ip   = argv[1];
    uint16_t    broker_port = static_cast<uint16_t>(std::atoi(argv[2]));
    int         num_pubs    = std::atoi(argv[3]);
    int         num_subs    = std::atoi(argv[4]);
    int         brake_limit = std::atoi(argv[5]);
    int         num_trials  = std::atoi(argv[6]);
    uint32_t    seed        = (argc >= 8) ? static_cast<uint32_t>(std::atoi(argv[7])) : 42;
    bool        encrypted   = (argc >= 9) ? (std::atoi(argv[8]) == 1) : false;

    srand(seed);

    Heps heps;
    if (encrypted) {
        heps.loadState("/tmp/dnspp_heps_full.key");
    }

    struct sockaddr_in broker_addr{};
    broker_addr.sin_family = AF_INET;
    broker_addr.sin_port   = htons(broker_port);
    inet_pton(AF_INET, broker_ip.c_str(), &broker_addr.sin_addr);

    std::cout << "trial,num_pubs,num_subs,brake_limit,encrypted,sub_id,sub_lat,sub_lon,"
              << "optimal_pub_id,optimal_dist,received_pub_id,received_dist,"
              << "stretch,recall,num_received,latency_ms" << std::endl;

    for (int t = 0; t < num_trials; t++) {
        std::string service = "bench_" + std::to_string(t);

        std::vector<Publisher> pubs(num_pubs);
        for (int i = 0; i < num_pubs; i++) {
            pubs[i].id  = i;
            pubs[i].lat = -90.0f + static_cast<float>(rand()) / RAND_MAX * 180.0f;
            pubs[i].lon = -180.0f + static_cast<float>(rand()) / RAND_MAX * 360.0f;
        }

        std::vector<Subscriber> subs(num_subs);
        for (int i = 0; i < num_subs; i++) {
            subs[i].id  = i;
            subs[i].lat = -90.0f + static_cast<float>(rand()) / RAND_MAX * 180.0f;
            subs[i].lon = -180.0f + static_cast<float>(rand()) / RAND_MAX * 360.0f;
        }

        for (auto& s : subs) {
            double min_dist = 1e18;
            int closest = -1;
            for (const auto& p : pubs) {
                double d = geoDistance(s.lat, s.lon, p.lat, p.lon);
                if (d < min_dist) { min_dist = d; closest = p.id; }
            }
            s.optimal_pub_id = closest;
            s.optimal_dist   = min_dist;
        }

        for (auto& s : subs) {
            s.fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port        = 0;
            bind(s.fd, (struct sockaddr*)&addr, sizeof(addr));

            TlvMessageBuilder builder(MsgType::SUBSCRIBE);
            builder.addServiceName(service);
            builder.addCoordinates(s.lat, s.lon);
            
            if (encrypted) {
                auto [bval_m1, bval_m2] = heps.blindSubscription(service);
                builder.addBlindedValue(bval_m1);
                builder.addBlindedValueHi(bval_m2);
            }
            
            auto pkt = builder.build();
            sendto(s.fd, pkt.data(), pkt.size(), 0,
                   (struct sockaddr*)&broker_addr, sizeof(broker_addr));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // --- Pre-compute blinded values (if encrypted) ---
        std::vector<std::string> bval_ns(pubs.size());
        if (encrypted) {
            for (size_t i = 0; i < pubs.size(); i++) {
                bval_ns[i] = heps.blindNotification(service);
            }
        }

        // --- Send publications ---
        auto publish_start = std::chrono::steady_clock::now();

        for (size_t i = 0; i < pubs.size(); i++) {
            const auto& p = pubs[i];
            TlvMessageBuilder builder(MsgType::PUBLISH);
            builder.addServiceName(service);
            builder.addCoordinates(p.lat, p.lon);
            builder.setPayload(std::to_string(p.id));
            
            if (encrypted) {
                builder.addBlindedValue(bval_ns[i]); // Use pre-computed value
            }
            
            auto pkt = builder.build();

            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            sendto(fd, pkt.data(), pkt.size(), 0,
                   (struct sockaddr*)&broker_addr, sizeof(broker_addr));
            close(fd);

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::vector<pollfd> pfds(subs.size());
        for (size_t i = 0; i < subs.size(); i++) {
            pfds[i].fd     = subs[i].fd;
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

        while (std::chrono::steady_clock::now() < deadline) {
            auto now = std::chrono::steady_clock::now();
            int remaining_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            if (remaining_ms <= 0) break;

            int ready = poll(pfds.data(), pfds.size(), remaining_ms);
            if (ready <= 0) break;

            for (size_t i = 0; i < pfds.size(); i++) {
                if (pfds[i].revents & POLLIN) {
                    uint8_t buf[4096];
                    int n = recv(pfds[i].fd, buf, sizeof(buf), 0);
                    if (n > 0) {
                        TlvMessage msg(buf, static_cast<size_t>(n));
                        if (msg.isValid() && msg.getMsgType() == MsgType::PUBLISH) {
                            std::string payload_str(
                                reinterpret_cast<const char*>(msg.getPayload()),
                                msg.getPayloadSize());
                            try {
                                int pub_id = std::stoi(payload_str);
                                auto recv_time = std::chrono::steady_clock::now();
                                double latency_ms = std::chrono::duration<double, std::milli>(
                                    recv_time - publish_start).count();
                                subs[i].received.push_back({pub_id, latency_ms});
                            } catch (...) {
                            }
                        }
                    }
                }
            }
        }

        int    total_recall   = 0;
        double total_stretch  = 0.0;
        int    total_received = 0;
        double total_latency  = 0.0;

        for (const auto& s : subs) {
            double best_dist = 1e18;
            int    best_pub  = -1;
            double best_latency = -1.0;

            for (const auto& rp : s.received) {
                if (rp.pub_id >= 0 && rp.pub_id < num_pubs) {
                    double d = geoDistance(s.lat, s.lon,
                                           pubs[rp.pub_id].lat, pubs[rp.pub_id].lon);
                    if (d < best_dist) { 
                        best_dist = d; 
                        best_pub = rp.pub_id; 
                        best_latency = rp.latency_ms;
                    }
                }
            }

            bool   recall  = (best_pub == s.optimal_pub_id);
            double stretch = (best_pub >= 0 && s.optimal_dist > 0)
                             ? best_dist / s.optimal_dist : -1.0;

            if (recall) total_recall++;
            if (best_pub >= 0) { total_stretch += stretch; total_received++; }
            if (best_latency >= 0) total_latency += best_latency;

            std::cout << t << ","
                      << num_pubs << "," << num_subs << "," << brake_limit << ","
                      << (encrypted ? 1 : 0) << ","
                      << s.id << "," << s.lat << "," << s.lon << ","
                      << s.optimal_pub_id << "," << s.optimal_dist << ","
                      << best_pub << ","
                      << (best_pub >= 0 ? best_dist : -1.0) << ","
                      << stretch << "," << (recall ? 1 : 0) << ","
                      << s.received.size() << ","
                      << best_latency << std::endl;
        }

        double avg_recall = static_cast<double>(total_recall) / subs.size();
        double avg_stretch = (total_received > 0)
                             ? total_stretch / total_received : -1.0;
        double avg_latency = (total_received > 0)
                             ? total_latency / total_received : -1.0;
                             
        std::cerr << "Trial " << t << ": recall=" << avg_recall
                  << " avg_stretch=" << avg_stretch
                  << " delivered=" << total_received << "/" << subs.size()
                  << " avg_latency=" << avg_latency << "ms"
                  << (encrypted ? " (Encrypted)" : " (Plaintext)")
                  << std::endl;

        // Broker 侧计数器：用来证明这次跑的到底是不是加密模式、订阅是否真的分了组。
        // 除 sub_groups / he_mode 是当前快照外，其余为 broker 启动以来的累计值。
        uint64_t st[10];
        if (queryStatsExt(broker_addr, st)) {
            std::cerr << "  broker stats (cumulative):";
            for (int i = 0; i < 10; i++) {
                std::cerr << " " << kStatNames[i] << "=" << st[i];
            }
            std::cerr << std::endl;
        } else {
            std::cerr << "  broker stats: UNAVAILABLE"
                      << " (no STATS_DATA_EXT in response -- broker not running,"
                      << " or built before TLV 0x0007)" << std::endl;
        }

        for (auto& s : subs) {
            if (s.fd >= 0) close(s.fd);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    return 0;
}