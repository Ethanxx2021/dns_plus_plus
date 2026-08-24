// benchmarks/bench_multi_broker.cpp
//
// Multi-broker benchmark: fork a 3-broker tree (root + 2 leaves), run N trials,
// each trial against a FRESH set of brokers (CLAUDE.md 不变量 I9:复用 broker 会污染
// sub_groups 累计计数器), and emit per-subscriber CSV + per-trial stderr summary.
//
// Usage:
//   ./bench_multi_broker <num_pubs> <num_subs> <brake_limit> <num_trials> [seed]
//                        [--warmup=<n>]
//
// Reproducibility:同 bench_broker,每个 trial 用 srand(seed + trial_index) 独立播种,
// seed / trial / trial_seed 写进 CSV 每一行。warm-up 轮用独立种子域并丢弃结果。

#include "protocol/TlvMessage.h"
#include "utils/geo.h"
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
#include <sys/wait.h>
#include <sys/time.h>
#include <endian.h>

namespace {

constexpr int kSubscriberRcvBuf = 4 * 1024 * 1024;   // 4 MiB,同 bench_broker
constexpr uint32_t kWarmupSeedOffset = 0x10000000u;  // warm-up 种子域偏移
constexpr int kDefaultWarmup = 3;

} // namespace

struct BrokerInfo {
    std::string id;
    uint16_t port;
    float lat, lon;
    std::string config_file;
};

struct Publisher {
    int id;
    float lat, lon;
    int broker_idx;
};

struct Subscriber {
    int id;
    float lat, lon;
    int broker_idx;
    int fd = -1;
    int optimal_pub_id = -1;
    double optimal_dist = 0.0;
    bool received = false;
    int received_pub_id = -1;
    double received_dist = 0.0;
};

void createBrokerConfig(const std::string& filename, const std::string& id,
                        uint16_t port, const std::string& parent_addr,
                        float lat, float lon, int brake_limit) {
    FILE* f = fopen(filename.c_str(), "w");
    if (!f) return;
    fprintf(f, "broker_id=%s\n", id.c_str());
    fprintf(f, "listen_port=%d\n", port);
    fprintf(f, "parent_addr=%s\n", parent_addr.c_str());
    fprintf(f, "coords=%f,%f\n", lat, lon);
    fprintf(f, "brake_limit=%d\n", brake_limit);
    fprintf(f, "brake_window=10\n");
    fprintf(f, "brake_scope=both\n");
    fprintf(f, "require_he=false\n");
    fclose(f);
}

pid_t startBroker(const std::string& binary, const std::string& config, int broker_index) {
    pid_t pid = fork();
    if (pid == 0) {
        std::string log_file = "/tmp/broker_" + std::to_string(broker_index) + ".log";
        freopen(log_file.c_str(), "w", stdout);
        freopen(log_file.c_str(), "w", stderr);
        execl(binary.c_str(), binary.c_str(), config.c_str(), nullptr);
        exit(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return pid;
}

void killBroker(pid_t pid) {
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
}

void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " <num_pubs> <num_subs> <brake_limit> <num_trials> [seed]"
              << " [--warmup=<n>]\n"
              << "\n"
              << "  seed         RNG seed (default 42). Each trial uses srand(seed + trial_index).\n"
              << "  brake_limit  written into each forked broker's config (per-quadrant limit).\n"
              << "  --warmup=<n> number of discarded warm-up rounds before timing (default "
              << kDefaultWarmup << ").\n";
}

int main(int argc, char* argv[]) {
    // ---- CLI 解析:positional 向后兼容,--warmup=<n> 新增 ----
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

    if (pos.size() < 4) {
        printUsage(argv[0]);
        return 1;
    }

    int num_pubs    = std::atoi(pos[0].c_str());
    int num_subs    = std::atoi(pos[1].c_str());
    int brake_limit = std::atoi(pos[2].c_str());
    int num_trials  = std::atoi(pos[3].c_str());
    uint32_t seed   = (pos.size() >= 5) ? static_cast<uint32_t>(std::atoi(pos[4].c_str())) : 42;

    if (num_pubs <= 0 || num_subs <= 0 || num_trials <= 0) {
        std::cerr << "FATAL: num_pubs/num_subs/num_trials must be > 0" << std::endl;
        return EXIT_FAILURE;
    }

    bench::printEnvironment();
    std::cerr << "[bench-run] num_pubs=" << num_pubs << " num_subs=" << num_subs
              << " brake_limit=" << brake_limit << " num_trials=" << num_trials
              << " seed=" << seed << " warmup=" << warmup << std::endl;

    bench::UdpSnmp snmp_start = bench::readUdpSnmp();
    if (!snmp_start.valid) {
        std::cerr << "[bench-net] WARNING: could not read /proc/net/snmp (Udp) at start; "
                     "RcvbufErrors/InErrors deltas will be unavailable" << std::endl;
    }

    // broker 二进制路径:benchmark 可能在仓库根目录(./build/dns_broker)或 build 目录
    // 内(./dns_broker)运行。旧的硬编码 "./dns_broker" 在仓库根下 execl 会静默失败、
    // 产出 recall 恒为 0 的假数据(CLAUDE.md:不静默降级)。两个候选都找不到就 fail-fast。
    std::string broker_bin;
    for (const char* c : {"./dns_broker", "./build/dns_broker"}) {
        if (access(c, X_OK) == 0) { broker_bin = c; break; }
    }
    if (broker_bin.empty()) {
        std::cerr << "FATAL: cannot find dns_broker binary (tried ./dns_broker, ./build/dns_broker). "
                  << "Run from the repository root or the build directory." << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<BrokerInfo> brokers = {
        {"broker_root",  9000, 0.0f, 0.0f, "/tmp/dnspp_root.conf"},
        {"broker_leaf1", 9001, 51.5f, -0.1f, "/tmp/dnspp_leaf1.conf"},
        {"broker_leaf2", 9002, 52.5f, 13.4f, "/tmp/dnspp_leaf2.conf"},
    };

    createBrokerConfig(brokers[0].config_file, brokers[0].id, brokers[0].port, "", brokers[0].lat, brokers[0].lon, brake_limit);
    createBrokerConfig(brokers[1].config_file, brokers[1].id, brokers[1].port, "127.0.0.1:9000", brokers[1].lat, brokers[1].lon, brake_limit);
    createBrokerConfig(brokers[2].config_file, brokers[2].id, brokers[2].port, "127.0.0.1:9000", brokers[2].lat, brokers[2].lon, brake_limit);

    std::cout << "seed,trial,trial_seed,num_pubs,num_subs,brake_limit,sub_id,sub_lat,sub_lon,"
              << "sub_broker,optimal_pub_id,optimal_dist,received,received_pub_id,"
              << "received_dist,stretch,recall" << std::endl;

    bool rcvbuf_reported = false;
    int round = 0;

    auto runTrial = [&](uint32_t trial_seed, int trial_index, bool emit) {
        srand(trial_seed);
        std::string service = "mbench_" + std::to_string(round++);

        // I9:每个 trial 起全新的 broker 进程,绝不复用上一轮的 broker。
        pid_t root_pid  = startBroker(broker_bin, brokers[0].config_file, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        pid_t leaf1_pid = startBroker(broker_bin, brokers[1].config_file, 1);
        pid_t leaf2_pid = startBroker(broker_bin, brokers[2].config_file, 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::vector<Publisher> pubs(num_pubs);
        for (int i = 0; i < num_pubs; i++) {
            pubs[i].id = i;
            pubs[i].lat = -90.0f + static_cast<float>(rand()) / RAND_MAX * 180.0f;
            pubs[i].lon = -180.0f + static_cast<float>(rand()) / RAND_MAX * 360.0f;
            double d1 = geoDistance(pubs[i].lat, pubs[i].lon, 51.5, -0.1);
            double d2 = geoDistance(pubs[i].lat, pubs[i].lon, 52.5, 13.4);
            pubs[i].broker_idx = (d1 < d2) ? 1 : 2;
        }

        std::vector<Subscriber> subs(num_subs);
        for (int i = 0; i < num_subs; i++) {
            subs[i].id = i;
            subs[i].lat = -90.0f + static_cast<float>(rand()) / RAND_MAX * 180.0f;
            subs[i].lon = -180.0f + static_cast<float>(rand()) / RAND_MAX * 360.0f;
            double d1 = geoDistance(subs[i].lat, subs[i].lon, 51.5, -0.1);
            double d2 = geoDistance(subs[i].lat, subs[i].lon, 52.5, 13.4);
            subs[i].broker_idx = (d1 < d2) ? 1 : 2;
        }

        for (auto& s : subs) {
            double min_dist = 1e18;
            int closest = -1;
            for (const auto& p : pubs) {
                double d = geoDistance(s.lat, s.lon, p.lat, p.lon);
                if (d < min_dist) { min_dist = d; closest = p.id; }
            }
            s.optimal_pub_id = closest;
            s.optimal_dist = min_dist;
        }

        for (auto& s : subs) {
            s.fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = 0;
            bind(s.fd, (struct sockaddr*)&addr, sizeof(addr));

            int actual = bench::setSubscriberRcvBuf(s.fd, kSubscriberRcvBuf);
            if (!rcvbuf_reported) {
                std::cerr << "[bench-net] subscriber_so_rcvbuf requested=" << kSubscriberRcvBuf
                          << " actual=" << actual
                          << (actual < 0 ? " (getsockopt FAILED)" : "") << std::endl;
                rcvbuf_reported = true;
            }

            struct sockaddr_in broker_addr{};
            broker_addr.sin_family = AF_INET;
            broker_addr.sin_port = htons(brokers[s.broker_idx].port);
            inet_pton(AF_INET, "127.0.0.1", &broker_addr.sin_addr);

            TlvMessageBuilder builder(MsgType::SUBSCRIBE);
            builder.addServiceName(service);
            builder.addCoordinates(s.lat, s.lon);
            auto pkt = builder.build();
            sendto(s.fd, pkt.data(), pkt.size(), 0, (struct sockaddr*)&broker_addr, sizeof(broker_addr));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        for (const auto& p : pubs) {
            struct sockaddr_in broker_addr{};
            broker_addr.sin_family = AF_INET;
            broker_addr.sin_port = htons(brokers[p.broker_idx].port);
            inet_pton(AF_INET, "127.0.0.1", &broker_addr.sin_addr);

            TlvMessageBuilder builder(MsgType::PUBLISH);
            builder.addServiceName(service);
            builder.addCoordinates(p.lat, p.lon);
            builder.setPayload(std::to_string(p.id));
            auto pkt = builder.build();

            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            sendto(fd, pkt.data(), pkt.size(), 0, (struct sockaddr*)&broker_addr, sizeof(broker_addr));
            close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::vector<pollfd> pfds(subs.size());
        for (size_t i = 0; i < subs.size(); i++) {
            pfds[i].fd = subs[i].fd;
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            auto now = std::chrono::steady_clock::now();
            int remaining_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            if (remaining_ms <= 0) break;

            int ready = poll(pfds.data(), pfds.size(), remaining_ms);
            if (ready <= 0) break;

            for (size_t i = 0; i < pfds.size(); i++) {
                if (pfds[i].revents & POLLIN) {
                    uint8_t buf[2048];
                    int n = recv(pfds[i].fd, buf, sizeof(buf), 0);
                    if (n > 0) {
                        TlvMessage msg(buf, static_cast<size_t>(n));
                        if (msg.isValid() && msg.getMsgType() == MsgType::PUBLISH) {
                            std::string payload_str(reinterpret_cast<const char*>(msg.getPayload()), msg.getPayloadSize());
                            try {
                                int pub_id = std::stoi(payload_str);
                                double d = geoDistance(subs[i].lat, subs[i].lon, pubs[pub_id].lat, pubs[pub_id].lon);
                                if (!subs[i].received || d < subs[i].received_dist) {
                                    subs[i].received = true;
                                    subs[i].received_pub_id = pub_id;
                                    subs[i].received_dist = d;
                                }
                            } catch (...) {}
                        }
                    }
                }
            }
        }

        int total_recall = 0;
        double total_stretch = 0.0;
        int total_received = 0;

        for (const auto& s : subs) {
            bool recall = (s.received_pub_id == s.optimal_pub_id);
            double stretch = (s.received && s.optimal_dist > 0) ? s.received_dist / s.optimal_dist : -1.0;

            if (recall) total_recall++;
            if (s.received) { total_stretch += stretch; total_received++; }

            if (emit) {
                std::cout << seed << "," << trial_index << "," << trial_seed << ","
                          << num_pubs << "," << num_subs << "," << brake_limit << ","
                          << s.id << "," << s.lat << "," << s.lon << ","
                          << s.broker_idx << "," << s.optimal_pub_id << "," << s.optimal_dist << ","
                          << (s.received ? 1 : 0) << "," << s.received_pub_id << ","
                          << (s.received ? s.received_dist : -1.0) << ","
                          << stretch << "," << (recall ? 1 : 0) << std::endl;
            }
        }

        double avg_recall = static_cast<double>(total_recall) / subs.size();
        double avg_stretch = (total_received > 0) ? total_stretch / total_received : -1.0;

        // --- Query traffic stats from all brokers (必须在 killBroker 之前) ---
        // warm-up 轮(emit=false)跳过统计与 summary,只做下面的关闭/清理。
        if (emit) {
        uint64_t total_up = 0, total_down = 0, total_local = 0, total_braked = 0;
        // STATS_DATA_EXT (TLV 0x0007) 的累计量与每 broker 快照量。
        // 解析它只需要 TlvMessage + endian.h，不引入任何密码学依赖。
        // 10 个累计槽：STATS_DATA_EXT 索引 0-7，外加 T3 新增的 braked_up/braked_local
        // (索引 10/11)。索引 8/9 (sub_groups/he_mode) 是快照量，不进这里。
        uint64_t ext_total[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        long ext_sub_groups[3] = {-1, -1, -1};   // -1 = 没拿到
        long ext_he_mode[3]    = {-1, -1, -1};
        int ext_ok = 0;
        for (int b_idx = 0; b_idx < 3; b_idx++) {
            int stat_fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in broker_addr{};
            broker_addr.sin_family = AF_INET;
            broker_addr.sin_port = htons(brokers[b_idx].port);
            inet_pton(AF_INET, "127.0.0.1", &broker_addr.sin_addr);

            TlvMessageBuilder req(MsgType::STATS_REQUEST);
            auto pkt = req.build();
            sendto(stat_fd, pkt.data(), pkt.size(), 0, (struct sockaddr*)&broker_addr, sizeof(broker_addr));

            uint8_t buf[2048];
            struct timeval tv = {1, 0};
            setsockopt(stat_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            int n = recv(stat_fd, buf, sizeof(buf), 0);
            if (n > 0) {
                TlvMessage msg(buf, n);
                if (msg.isValid() && msg.getMsgType() == MsgType::STATS_RESPONSE) {
                    uint16_t len;
                    const uint8_t* v = msg.findTlv(TlvType::STATS_DATA, &len);
                    if (v && len >= 32) {
                        uint64_t up, down, local, braked;
                        std::memcpy(&up, v, 8); up = be64toh(up);
                        std::memcpy(&down, v+8, 8); down = be64toh(down);
                        std::memcpy(&local, v+16, 8); local = be64toh(local);
                        std::memcpy(&braked, v+24, 8); braked = be64toh(braked);
                        total_up += up;
                        total_down += down;
                        total_local += local;
                        total_braked += braked;
                    }

                    uint16_t elen;
                    const uint8_t* e = msg.findTlv(TlvType::STATS_DATA_EXT, &elen);
                    // T3 起 STATS_DATA_EXT 是 96 字节 (12 uint64)；仍兼容 80 字节的旧 broker。
                    if (e && elen >= 80) {
                        int nfields = (elen >= 96) ? 12 : 10;
                        uint64_t vals[12] = {0};
                        for (int i = 0; i < nfields; i++) {
                            uint64_t be;
                            std::memcpy(&be, e + i * 8, 8);
                            vals[i] = be64toh(be);
                        }
                        // 索引 0-7 是累计量，跨 broker 相加
                        for (int i = 0; i < 8; i++) ext_total[i] += vals[i];
                        // 索引 10/11 (braked_up/braked_local) 也是累计量
                        ext_total[8] += vals[10];   // braked_up
                        ext_total[9] += vals[11];   // braked_local
                        // 索引 8/9 (sub_groups/he_mode) 是每 broker 的快照量，分别保留
                        ext_sub_groups[b_idx] = static_cast<long>(vals[8]);
                        ext_he_mode[b_idx]    = static_cast<long>(vals[9]);
                        ext_ok++;
                    }
                }
            }
            close(stat_fd);
        }

        uint64_t total_traffic = total_up + total_down + total_local;
        double traffic_ratio = (total_local > 0) ? (double)total_traffic / total_local : 0.0;

        std::cerr << "Trial " << trial_index << ": recall=" << avg_recall
                  << " avg_stretch=" << avg_stretch
                  << " delivered=" << total_received << "/" << subs.size()
                  << " | Traffic: up=" << total_up << " down=" << total_down
                  << " local=" << total_local << " braked=" << total_braked
                  << " | Traffic Ratio=" << traffic_ratio << std::endl;

        // Broker 侧计数器。3 个 broker 全部应答才算完整；缺一个都必须看得出来。
        static const char* kExtNames[10] = {
            "forward_up", "forward_down", "delivered_local", "braked",
            "match_calls", "match_hits", "pub_received", "sub_received",
            "braked_up", "braked_local"
        };
        std::cerr << "  broker stats (sum over " << ext_ok << "/3 brokers, cumulative):";
        for (int i = 0; i < 10; i++) {
            std::cerr << " " << kExtNames[i] << "=" << ext_total[i];
        }
        std::cerr << std::endl;
        std::cerr << "  per-broker snapshot (root,leaf1,leaf2): sub_groups=["
                  << ext_sub_groups[0] << "," << ext_sub_groups[1] << "," << ext_sub_groups[2]
                  << "] he_mode=["
                  << ext_he_mode[0] << "," << ext_he_mode[1] << "," << ext_he_mode[2]
                  << "]  (-1 = no STATS_DATA_EXT in response)" << std::endl;
        } // if (emit)

        for (auto& s : subs) {
            if (s.fd >= 0) close(s.fd);
        }
        killBroker(leaf1_pid);
        killBroker(leaf2_pid);
        killBroker(root_pid);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    };

    // ---- warm-up ----
    for (int w = 0; w < warmup; w++) {
        runTrial(seed + kWarmupSeedOffset + static_cast<uint32_t>(w), -1, false);
    }

    // ---- 正式 trial ----
    for (int t = 0; t < num_trials; t++) {
        uint32_t trial_seed = seed + static_cast<uint32_t>(t);
        runTrial(trial_seed, t, true);
    }

    // ---- UDP 丢包统计 ----
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
