// benchmarks/bench_broker.cpp
//
// Automated benchmark for DNS++ Phase 1/3 (single broker, plaintext/encrypted).
//
// Usage:
//   ./bench_broker <broker_ip> <broker_port> <num_pubs> <num_subs> <brake_limit>
//                  <num_trials> [seed] [encrypted=0/1] [--warmup=<n>]
//
// Reproducibility (论文 Experiments 复现性要求):
//   * seed 来自命令行(缺省 42)。
//   * 每个 trial 用 srand(seed + trial_index) 重新播种 —— 不是所有 trial 共用
//     一个种子(旧 bug 会让 N 个 trial 变成同一份数据的 N 次重复,标准差无意义)。
//   * warm-up 轮用独立的种子域(seed + kWarmupSeedOffset + w),与正式 trial 的
//     "seed + trial_index" 域隔离;因为 srand 每次都会重置状态,warm-up 的播种
//     无论如何都不会影响正式 trial 的可复现性。
//   * seed / trial_index / trial_seed 都写进 CSV 的每一行。
//
// Output:
//   stdout: CSV (per-subscriber detail, one row per subscriber per measured trial)
//   stderr: environment metadata, UDP snmp deltas, SO_RCVBUF readback, summary

#include "protocol/TlvMessage.h"
#include "utils/geo.h"
#include "crypto/Heps.h"
#include "bench_common.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <thread>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <endian.h>

namespace {

// 订阅者 socket 请求的接收缓冲区大小(4 MiB)。UDP 高吞吐下默认 ~212KB 的缓冲
// 很容易被打满从而产生内核 RcvbufErrors,把"算法过滤"和"内核丢包"混在一起。
constexpr int kSubscriberRcvBuf = 4 * 1024 * 1024;

// warm-up 轮种子偏移:把 warm-up 的种子域从 "seed + trial_index" 里隔开,
// 避免读者把某个 warm-up 轮误当成某个正式 trial。数值本身无数学含义。
constexpr uint32_t kWarmupSeedOffset = 0x10000000u;

constexpr int kDefaultWarmup = 3;

} // namespace

struct Publisher {
    int id;
    float lat;
    float lon;
};

// STATS_DATA_EXT (TLV 0x0007): T3 起为 12 x uint64_t big-endian (96 字节)。
// 顺序与 broker 的 handleStatsRequest() 一致。索引 3 的 braked 是合计
// (= braked_up + braked_local)，索引 10/11 给出拆分。
static const int   kNumStats = 12;
static const char* kStatNames[kNumStats] = {
    "forward_up", "forward_down", "delivered_local", "braked",
    "match_calls", "match_hits", "pub_received", "sub_received",
    "sub_groups", "he_mode", "braked_up", "braked_local"
};

// 向 broker 请求扩展统计。返回 false 表示没拿到（broker 没响应，或者是一个
// 还不认识 0x0007 的旧 broker）—— 调用方必须把这种情况显式报出来，
// 不能当成「计数器全 0」。
bool queryStatsExt(const struct sockaddr_in& broker_addr, uint64_t out[kNumStats]) {
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

    // 兼容 80 字节的旧 broker：缺的两个拆分字段填 0。
    int navail = (len >= 96) ? 12 : 10;
    for (int i = 0; i < kNumStats; i++) out[i] = 0;
    for (int i = 0; i < navail; i++) {
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
        double latency_ms;         // 端到端:从第一条 PUBLISH 发出到收到(旧指标,保留)
        double latency_per_pub_ms; // 新指标:从本条 PUBLISH 自己发出到收到(扣除调度间隔)
    };
    std::vector<ReceivedPub> received;
};

void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " <broker_ip> <broker_port> <num_pubs> <num_subs>"
              << " <brake_limit> <num_trials> [seed] [encrypted=0/1]"
              << " [--warmup=<n>]\n"
              << "\n"
              << "  seed        RNG seed (default 42). Each trial uses srand(seed + trial_index).\n"
              << "  encrypted   0=plaintext (default), 1=encrypted (requires /tmp/dnspp_heps_full.key).\n"
              << "  --warmup=<n>  number of discarded warm-up rounds before timing (default "
              << kDefaultWarmup << ").\n";
}

int main(int argc, char* argv[]) {
    // ---- CLI 解析:positional 参数保持向后兼容,--warmup=<n> 为新增可选参数 ----
    int warmup = kDefaultWarmup;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (a.rfind("--warmup=", 0) == 0) {
            warmup = std::atoi(a.c_str() + strlen("--warmup="));
            if (warmup < 0) {
                std::cerr << "FATAL: --warmup must be >= 0, got \"" << a << "\"" << std::endl;
                return EXIT_FAILURE;
            }
        } else {
            pos.push_back(a);
        }
    }

    if (pos.size() < 6) {
        printUsage(argv[0]);
        return 1;
    }

    std::string broker_ip   = pos[0];
    uint16_t    broker_port = static_cast<uint16_t>(std::atoi(pos[1].c_str()));
    int         num_pubs    = std::atoi(pos[2].c_str());
    int         num_subs    = std::atoi(pos[3].c_str());
    int         brake_limit = std::atoi(pos[4].c_str());
    int         num_trials  = std::atoi(pos[5].c_str());
    uint32_t    seed        = (pos.size() >= 7) ? static_cast<uint32_t>(std::atoi(pos[6].c_str())) : 42;
    bool        encrypted   = (pos.size() >= 8) ? (std::atoi(pos[7].c_str()) == 1) : false;

    if (num_pubs <= 0 || num_subs <= 0 || num_trials <= 0) {
        std::cerr << "FATAL: num_pubs/num_subs/num_trials must be > 0" << std::endl;
        return EXIT_FAILURE;
    }

    // ---- 环境元数据 + UDP 丢包基线 ----
    bench::printEnvironment();
    std::cerr << "[bench-run] broker=" << broker_ip << ":" << broker_port
              << " num_pubs=" << num_pubs << " num_subs=" << num_subs
              << " brake_limit=" << brake_limit << " num_trials=" << num_trials
              << " seed=" << seed << " encrypted=" << (encrypted ? 1 : 0)
              << " warmup=" << warmup << std::endl;

    bench::UdpSnmp snmp_start = bench::readUdpSnmp();
    if (!snmp_start.valid) {
        std::cerr << "[bench-net] WARNING: could not read /proc/net/snmp (Udp) at start; "
                     "RcvbufErrors/InErrors deltas will be unavailable" << std::endl;
    }

    Heps heps;
    if (encrypted) {
        if (!heps.loadState("/tmp/dnspp_heps_full.key")) {
            std::cerr << "FATAL: cannot load HEPS key file. "
                      << "Start the root broker first, or pass encrypted=0."
                      << std::endl;
            return EXIT_FAILURE;
        }
    }

    struct sockaddr_in broker_addr{};
    broker_addr.sin_family = AF_INET;
    broker_addr.sin_port   = htons(broker_port);
    inet_pton(AF_INET, broker_ip.c_str(), &broker_addr.sin_addr);

    std::cout << "seed,trial,trial_seed,num_pubs,num_subs,brake_limit,encrypted,"
              << "sub_id,sub_lat,sub_lon,optimal_pub_id,optimal_dist,"
              << "received_pub_id,received_dist,stretch,recall,num_received,"
              << "latency_ms,latency_per_pub_ms" << std::endl;

    bool rcvbuf_reported = false;

    // 单个 trial(或 warm-up 轮)的完整流程。emit=false 时丢弃 CSV 行与 summary。
    // trial_index:正式 trial 从 0 起;warm-up 轮传 -1(只用于不打印)。
    // round:全局轮次号,只用于生成每轮唯一的 service 名(避免同 broker 串组)。
    int round = 0;
    auto runTrial = [&](uint32_t trial_seed, int trial_index, bool emit) {
        srand(trial_seed);
        std::string service = "bench_" + std::to_string(round++);

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

            int actual = bench::setSubscriberRcvBuf(s.fd, kSubscriberRcvBuf);
            if (!rcvbuf_reported) {
                std::cerr << "[bench-net] subscriber_so_rcvbuf requested=" << kSubscriberRcvBuf
                          << " actual=" << actual
                          << (actual < 0 ? " (getsockopt FAILED)" : "") << std::endl;
                rcvbuf_reported = true;
            }

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

        // --- Send publications, recording each pub's own send time ---
        auto publish_start = std::chrono::steady_clock::now();
        std::vector<std::chrono::steady_clock::time_point> pub_send_time(pubs.size());

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
            pub_send_time[i] = std::chrono::steady_clock::now();
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
                                double latency_per_pub_ms = -1.0;
                                if (pub_id >= 0 && pub_id < num_pubs) {
                                    latency_per_pub_ms = std::chrono::duration<double, std::milli>(
                                        recv_time - pub_send_time[pub_id]).count();
                                }
                                subs[i].received.push_back({pub_id, latency_ms, latency_per_pub_ms});
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
        double total_latency_per_pub = 0.0;

        for (const auto& s : subs) {
            double best_dist = 1e18;
            int    best_pub  = -1;
            double best_latency = -1.0;
            double best_latency_per_pub = -1.0;

            for (const auto& rp : s.received) {
                if (rp.pub_id >= 0 && rp.pub_id < num_pubs) {
                    double d = geoDistance(s.lat, s.lon,
                                           pubs[rp.pub_id].lat, pubs[rp.pub_id].lon);
                    if (d < best_dist) {
                        best_dist = d;
                        best_pub = rp.pub_id;
                        best_latency = rp.latency_ms;
                        best_latency_per_pub = rp.latency_per_pub_ms;
                    }
                }
            }

            bool   recall  = (best_pub == s.optimal_pub_id);
            double stretch = (best_pub >= 0 && s.optimal_dist > 0)
                             ? best_dist / s.optimal_dist : -1.0;

            if (recall) total_recall++;
            if (best_pub >= 0) { total_stretch += stretch; total_received++; }
            if (best_latency >= 0) total_latency += best_latency;
            if (best_latency_per_pub >= 0) total_latency_per_pub += best_latency_per_pub;

            if (emit) {
                std::cout << seed << "," << trial_index << "," << trial_seed << ","
                          << num_pubs << "," << num_subs << "," << brake_limit << ","
                          << (encrypted ? 1 : 0) << ","
                          << s.id << "," << s.lat << "," << s.lon << ","
                          << s.optimal_pub_id << "," << s.optimal_dist << ","
                          << best_pub << ","
                          << (best_pub >= 0 ? best_dist : -1.0) << ","
                          << stretch << "," << (recall ? 1 : 0) << ","
                          << s.received.size() << ","
                          << best_latency << "," << best_latency_per_pub << std::endl;
            }
        }

        for (auto& s : subs) {
            if (s.fd >= 0) close(s.fd);
        }

        if (!emit) return;

        double avg_recall = static_cast<double>(total_recall) / subs.size();
        double avg_stretch = (total_received > 0)
                             ? total_stretch / total_received : -1.0;
        double avg_latency = (total_received > 0)
                             ? total_latency / total_received : -1.0;
        double avg_latency_per_pub = (total_received > 0)
                             ? total_latency_per_pub / total_received : -1.0;

        std::cerr << "Trial " << trial_index << ": recall=" << avg_recall
                  << " avg_stretch=" << avg_stretch
                  << " delivered=" << total_received << "/" << subs.size()
                  << " avg_latency=" << avg_latency << "ms"
                  << " avg_latency_per_pub=" << avg_latency_per_pub << "ms"
                  << (encrypted ? " (Encrypted)" : " (Plaintext)")
                  << std::endl;

        // Broker 侧计数器：用来证明这次跑的到底是不是加密模式、订阅是否真的分了组。
        // 除 sub_groups / he_mode 是当前快照外，其余为 broker 启动以来的累计值。
        uint64_t st[kNumStats];
        if (queryStatsExt(broker_addr, st)) {
            std::cerr << "  broker stats (cumulative):";
            for (int i = 0; i < kNumStats; i++) {
                std::cerr << " " << kStatNames[i] << "=" << st[i];
            }
            std::cerr << std::endl;
        } else {
            std::cerr << "  broker stats: UNAVAILABLE"
                      << " (no STATS_DATA_EXT in response -- broker not running,"
                      << " or built before TLV 0x0007)" << std::endl;
        }
    };

    // ---- warm-up:正式计时前丢弃结果的轮次,让 CPU 频率与内存分配器进入稳态 ----
    for (int w = 0; w < warmup; w++) {
        runTrial(seed + kWarmupSeedOffset + static_cast<uint32_t>(w), -1, false);
    }

    // ---- 正式 trial:每个 trial 用 seed + trial_index 独立播种 ----
    for (int t = 0; t < num_trials; t++) {
        uint32_t trial_seed = seed + static_cast<uint32_t>(t);
        runTrial(trial_seed, t, true);
    }

    // ---- 结束时的 UDP 丢包统计(与开始时做差) ----
    bench::UdpSnmp snmp_end = bench::readUdpSnmp();
    if (snmp_start.valid && snmp_end.valid) {
        std::cerr << "[bench-net] udp_rcvbuf_errors_delta="
                  << (snmp_end.rcvbuf_errors - snmp_start.rcvbuf_errors)
                  << " udp_in_errors_delta="
                  << (snmp_end.in_errors - snmp_start.in_errors)
                  << " (start: rcvbuf=" << snmp_start.rcvbuf_errors
                  << " inerr=" << snmp_start.in_errors
                  << "; end: rcvbuf=" << snmp_end.rcvbuf_errors
                  << " inerr=" << snmp_end.in_errors << ")" << std::endl;
    }

    return 0;
}
