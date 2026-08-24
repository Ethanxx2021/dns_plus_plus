// benchmarks/bench_dynamics.cpp
//
// Dynamics benchmark:测量「服务副本迁移后订阅者多快收敛到新副本」。
//
// 论文中心假设「服务迁移时更新延迟是秒级而非分钟级」此前没有任何数字支撑;
// 这个 benchmark 补上这块实验证据。
//
// Usage:
//   ./bench_dynamics <broker_ip> <broker_port> <num_pubs> <num_subs>
//                    <num_migrations> <trials> <seed> [encrypted=0/1]
//                    [--timeout-ms=<n>]
//
// 每个 trial 用 srand(seed + trial_index) 播种(与 bench_broker 一致),订阅者与
// 发布者用与 bench_broker 完全相同的均匀经纬度采样。bench_dynamics 本身只连接一个
// 已运行的 broker(和 bench_broker 一致);「每个 trial / 每组参数起全新 broker」
// 由驱动脚本负责(不变量 I9)。
//
// 核心测量约束(Algorithm 1 的 per-subscriber closest filter):
//   订阅者的 cached_closest_dist 只降不升。迁移后若新副本【比当前最近副本更近】,
//   broker 会投递(能测到收敛);若原本最近的副本【变远了】,订阅者的真实最优变成
//   另一个副本,而那个副本之前未必被投递过,broker 也不会主动推它 → 可能不收敛。
//   这两类用 migration_class 区分("closer" / "farther"),分别统计。
//
// Output:
//   stdout: CSV,每行一个 (trial, migration, subscriber) —— 仅当该订阅者的
//           ground-truth 最近副本在此次迁移中发生了变化。
//   stderr: 环境元数据、UDP 丢包差值、按 class 分组的收敛率/延迟分布。

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
#include <functional>
#include <algorithm>
#include <numeric>
#include <poll.h>
#include <sys/time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <endian.h>

namespace {

constexpr int kSubscriberRcvBuf = 4 * 1024 * 1024;   // 同 bench_broker
constexpr int kDefaultTimeoutMs = 5000;              // 收敛超时,可配置

// 与 bench_broker 完全一致的均匀经纬度采样(在 lat/lon 度上均匀)。
float uniformLat() { return -90.0f + static_cast<float>(rand()) / RAND_MAX * 180.0f; }
float uniformLon() { return -180.0f + static_cast<float>(rand()) / RAND_MAX * 360.0f; }

} // namespace

struct Pub {
    int id;
    float lat, lon;
};

struct Sub {
    int id;
    float lat, lon;
    int fd = -1;
    std::vector<bool> seen;   // seen[pub_id] == true 表示该订阅者收到过该副本
    double cached_dist = 1e18; // 镜像 broker 侧 GeoClient.cached_closest_dist(只降不升)
    int closest_pub = -1;       // 达到 cached_dist 的那个副本 id
};

void printUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " <broker_ip> <broker_port> <num_pubs> <num_subs>"
              << " <num_migrations> <trials> <seed> [encrypted=0/1]"
              << " [--timeout-ms=<n>]\n"
              << "\n"
              << "  seed            RNG seed (default 42). Each trial uses srand(seed + trial_index).\n"
              << "  encrypted       0=plaintext (default), 1=encrypted (requires /tmp/dnspp_heps_full.key).\n"
              << "  --timeout-ms=<n> convergence timeout in ms (default " << kDefaultTimeoutMs << ").\n";
}

// 某订阅者当前 ground-truth 最近的副本 id。
int closestPub(const Sub& s, const std::vector<Pub>& pubs) {
    int best = -1;
    double best_d = 1e18;
    for (const auto& p : pubs) {
        double d = geoDistance(s.lat, s.lon, p.lat, p.lon);
        if (d < best_d) { best_d = d; best = p.id; }
    }
    return best;
}

// 查询 broker 的 delivered_local / braked 计数(STATS_DATA_EXT 索引 2/3)。
// 用于检测 brake 是否过滤了 publication —— brake 会污染 dynamics 测量:把「被 brake
// 拦下」误当成「Algorithm 1 未收敛」。返回是否成功。
bool queryBrakeCounters(const struct sockaddr_in& broker_addr,
                        uint64_t& delivered_local, uint64_t& braked) {
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
    if (!v || len < 32) return false;   // 至少需要前 4 个 uint64

    uint64_t vals[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        uint64_t be;
        std::memcpy(&be, v + i * 8, 8);
        vals[i] = be64toh(be);
    }
    delivered_local = vals[2];
    braked = vals[3];
    return true;
}

// 轮询订阅者 socket 直到 deadline;每收到一条 PUBLISH 回调 cb(sub_idx, pub_id, t_recv)。
void drainSubscribers(std::vector<Sub>& subs,
                      std::chrono::steady_clock::time_point deadline,
                      const std::function<void(int, int, std::chrono::steady_clock::time_point)>& cb) {
    std::vector<pollfd> pfds(subs.size());
    for (size_t i = 0; i < subs.size(); i++) {
        pfds[i].fd = subs[i].fd;
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }

    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (remaining <= 0) break;

        int ready = poll(pfds.data(), pfds.size(), static_cast<int>(remaining));
        if (ready <= 0) break;

        for (size_t i = 0; i < pfds.size(); i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            uint8_t buf[4096];
            int n = recv(pfds[i].fd, buf, sizeof(buf), 0);
            if (n <= 0) continue;
            TlvMessage msg(buf, static_cast<size_t>(n));
            if (!msg.isValid() || msg.getMsgType() != MsgType::PUBLISH) continue;
            if (!msg.getPayload() || msg.getPayloadSize() == 0) continue;
            std::string payload(reinterpret_cast<const char*>(msg.getPayload()),
                                msg.getPayloadSize());
            int pub_id = -1;
            try { pub_id = std::stoi(payload); } catch (...) { continue; }
            cb(static_cast<int>(i), pub_id, std::chrono::steady_clock::now());
        }
    }
}

int main(int argc, char* argv[]) {
    // ---- CLI 解析:positional 向后兼容,--timeout-ms=<n> 可选 ----
    int timeout_ms = kDefaultTimeoutMs;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (a.rfind("--timeout-ms=", 0) == 0) {
            timeout_ms = std::atoi(a.c_str() + strlen("--timeout-ms="));
            if (timeout_ms <= 0) {
                std::cerr << "FATAL: --timeout-ms must be > 0, got \"" << a << "\"" << std::endl;
                return EXIT_FAILURE;
            }
        } else {
            pos.push_back(a);
        }
    }

    if (pos.size() < 7) {
        printUsage(argv[0]);
        return 1;
    }

    std::string broker_ip   = pos[0];
    uint16_t    broker_port = static_cast<uint16_t>(std::atoi(pos[1].c_str()));
    int         num_pubs    = std::atoi(pos[2].c_str());
    int         num_subs    = std::atoi(pos[3].c_str());
    int         num_migr    = std::atoi(pos[4].c_str());
    int         num_trials  = std::atoi(pos[5].c_str());
    uint32_t    seed        = static_cast<uint32_t>(std::atoi(pos[6].c_str()));
    bool        encrypted   = (pos.size() >= 8) ? (std::atoi(pos[7].c_str()) == 1) : false;

    if (num_pubs <= 0 || num_subs <= 0 || num_migr <= 0 || num_trials <= 0) {
        std::cerr << "FATAL: num_pubs/num_subs/num_migrations/trials must be > 0" << std::endl;
        return EXIT_FAILURE;
    }
    if (num_pubs < 2) {
        std::cerr << "FATAL: num_pubs must be >= 2 (a migration needs at least one other "
                     "replica to become the new closest)" << std::endl;
        return EXIT_FAILURE;
    }

    // ---- 环境元数据 + UDP 丢包基线 ----
    bench::printEnvironment();
    std::cerr << "[bench-run] broker=" << broker_ip << ":" << broker_port
              << " num_pubs=" << num_pubs << " num_subs=" << num_subs
              << " num_migrations=" << num_migr << " trials=" << num_trials
              << " seed=" << seed << " encrypted=" << (encrypted ? 1 : 0)
              << " timeout_ms=" << timeout_ms << std::endl;

    bench::UdpSnmp snmp_start = bench::readUdpSnmp();
    if (!snmp_start.valid) {
        std::cerr << "[bench-net] WARNING: could not read /proc/net/snmp (Udp) at start" << std::endl;
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

    std::cout << "trial,migration_idx,sub_id,seed,trial_seed,encrypted,"
              << "old_closest_pub,new_optimal_pub,received_pub,"
              << "converged,convergence_ms,migration_class" << std::endl;

    // 汇总累计(跨所有 trial,按 class 分组)
    long closer_total = 0, closer_converged = 0;
    long farther_total = 0, farther_converged = 0;
    long farther_new_opt_seen_before = 0;   // farther 中「新最优副本之前已被投递过」的数量
    std::vector<double> closer_ms, farther_ms;

    bool rcvbuf_reported = false;
    int round = 0;

    auto runTrial = [&](uint32_t trial_seed, int trial_index) {
        srand(trial_seed);
        std::string service = "dyn_" + std::to_string(round++);

        std::vector<Pub> pubs(num_pubs);
        for (int i = 0; i < num_pubs; i++) {
            pubs[i].id = i;
            pubs[i].lat = uniformLat();
            pubs[i].lon = uniformLon();
        }

        std::vector<Sub> subs(num_subs);
        for (int i = 0; i < num_subs; i++) {
            subs[i].id = i;
            subs[i].lat = uniformLat();
            subs[i].lon = uniformLon();
            subs[i].seen.assign(num_pubs, false);
        }

        // ---- 订阅 ----
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

            TlvMessageBuilder b(MsgType::SUBSCRIBE);
            b.addServiceName(service);
            b.addCoordinates(s.lat, s.lon);
            if (encrypted) {
                auto [m1, m2] = heps.blindSubscription(service);
                b.addBlindedValue(m1);
                b.addBlindedValueHi(m2);
            }
            auto pkt = b.build();
            sendto(s.fd, pkt.data(), pkt.size(), 0,
                   (const struct sockaddr*)&broker_addr, sizeof(broker_addr));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // bval_n 只依赖服务名(blindNotification(service)),对该服务的所有 publication
        // 都有效;Match 对密文身份无状态,整轮复用同一份即可。
        std::string bval_n;
        if (encrypted) bval_n = heps.blindNotification(service);

        // ---- 初始发布:所有副本各发一次 ----
        for (const auto& p : pubs) {
            TlvMessageBuilder b(MsgType::PUBLISH);
            b.addServiceName(service);
            b.addCoordinates(p.lat, p.lon);
            b.setPayload(std::to_string(p.id));
            if (encrypted) b.addBlindedValue(bval_n);
            auto pkt = b.build();
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            sendto(fd, pkt.data(), pkt.size(), 0,
                   (const struct sockaddr*)&broker_addr, sizeof(broker_addr));
            close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // 订阅者收到 pub 时,同步更新 seen 与「镜像的 cached_closest_dist」。
        // 注意:d 用当前 pub 位置计算;每个 drain 窗口内位置固定,所以等价于 broker
        // 在投递时刻用当时位置算的距离。
        auto noteReceive = [&](int si, int pub_id) {
            if (pub_id < 0 || pub_id >= num_pubs) return;
            subs[si].seen[pub_id] = true;
            double d = geoDistance(subs[si].lat, subs[si].lon,
                                   pubs[pub_id].lat, pubs[pub_id].lon);
            if (d < subs[si].cached_dist) {
                subs[si].cached_dist = d;
                subs[si].closest_pub = pub_id;
            }
        };

        // ---- 等待收敛(500ms),排空初始发布,记录已收到的副本 ----
        {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            drainSubscribers(subs, deadline, [&](int si, int pub_id, auto) {
                noteReceive(si, pub_id);
            });
        }

        // ---- 迁移循环 ----
        for (int mig_idx = 0; mig_idx < num_migr; mig_idx++) {
            // 随机选一个发布者,移到新随机位置
            int mig = rand() % num_pubs;
            pubs[mig].lat = uniformLat();
            pubs[mig].lon = uniformLon();

            // 分类依据是 broker 的投递 filter:dist(mig_new) < cached_closest_dist 才投递。
            //   closer  = mig 新位置比订阅者当前 cached 最近副本更近 → 会被投递,能测收敛
            //   farther = mig 之前就是该订阅者的 cached 最近副本,现在变远 → 最优换成别的
            //   (其余 = 未变化,不测量)
            std::vector<int> target(num_subs, -1);      // 收敛目标 = 迁移后的 ground-truth 最近副本
            std::vector<int> old_closest(num_subs, -1);
            std::vector<std::string> cls(num_subs);
            std::vector<bool> new_opt_seen_before(num_subs, false);
            int n_closer = 0, n_farther = 0;
            for (int i = 0; i < num_subs; i++) {
                double d_new = geoDistance(subs[i].lat, subs[i].lon,
                                           pubs[mig].lat, pubs[mig].lon);
                if (d_new < subs[i].cached_dist) {
                    // 更近 → 投递;新最优就是 mig 本身
                    cls[i] = "closer";
                    target[i] = mig;
                    old_closest[i] = subs[i].closest_pub;
                    n_closer++;
                } else if (subs[i].closest_pub == mig) {
                    // mig 之前是最优,现在变远 → 最优换成别的副本
                    cls[i] = "farther";
                    target[i] = closestPub(subs[i], pubs);
                    old_closest[i] = subs[i].closest_pub;   // == mig
                    new_opt_seen_before[i] = subs[i].seen[target[i]];
                    n_farther++;
                }
            }

            // 发送迁移后的 PUBLISH,记录 t_send
            auto t_send = std::chrono::steady_clock::now();
            {
                TlvMessageBuilder b(MsgType::PUBLISH);
                b.addServiceName(service);
                b.addCoordinates(pubs[mig].lat, pubs[mig].lon);
                b.setPayload(std::to_string(mig));
                if (encrypted) b.addBlindedValue(bval_n);
                auto pkt = b.build();
                int fd = socket(AF_INET, SOCK_DGRAM, 0);
                sendto(fd, pkt.data(), pkt.size(), 0,
                       (const struct sockaddr*)&broker_addr, sizeof(broker_addr));
                close(fd);
            }

            // 轮询直到超时,记录到达
            std::vector<bool> converged(num_subs, false);
            std::vector<double> conv_ms(num_subs, -1.0);
            std::vector<int> received_pub(num_subs, -1);
            {
                auto deadline = t_send + std::chrono::milliseconds(timeout_ms);
                drainSubscribers(subs, deadline, [&](int si, int pub_id, auto t_recv) {
                    noteReceive(si, pub_id);
                    received_pub[si] = pub_id;
                    if (target[si] >= 0 && pub_id == target[si] && !converged[si]) {
                        converged[si] = true;
                        conv_ms[si] = std::chrono::duration<double, std::milli>(t_recv - t_send).count();
                    }
                });
            }

            // 输出 CSV + 累计
            for (int i = 0; i < num_subs; i++) {
                if (target[i] < 0) continue;   // 未变化,无记录
                bool conv = converged[i];
                double ms = conv ? conv_ms[i] : -1.0;

                std::cout << trial_index << "," << mig_idx << "," << i << ","
                          << seed << "," << trial_seed << ","
                          << (encrypted ? 1 : 0) << ","
                          << old_closest[i] << "," << target[i] << ","
                          << received_pub[i] << ","
                          << (conv ? 1 : 0) << "," << ms << ","
                          << cls[i] << std::endl;

                if (cls[i] == "closer") {
                    closer_total++;
                    if (conv) { closer_converged++; closer_ms.push_back(ms); }
                } else {
                    farther_total++;
                    if (conv) { farther_converged++; farther_ms.push_back(ms); }
                    if (new_opt_seen_before[i]) farther_new_opt_seen_before++;
                }
            }

            std::cerr << "Trial " << trial_index << " migration " << mig_idx
                      << " (moved pub " << mig << "):"
                      << " closer=" << n_closer
                      << " farther=" << n_farther
                      << std::endl;
        }

        for (auto& s : subs) {
            if (s.fd >= 0) close(s.fd);
        }
    };

    // ---- 正式 trial:每个 trial 用 seed + trial_index 独立播种 ----
    for (int t = 0; t < num_trials; t++) {
        uint32_t trial_seed = seed + static_cast<uint32_t>(t);
        runTrial(trial_seed, t);
    }

    // ---- 汇总(按 class 分组) ----
    auto summarize = [&](const char* name, long total, long conv,
                         const std::vector<double>& ms, long seen_before) {
        std::vector<double> v = ms;
        std::sort(v.begin(), v.end());
        double rate = (total > 0) ? (double)conv / total : 0.0;
        double mean = v.empty() ? -1.0
                     : std::accumulate(v.begin(), v.end(), 0.0) / v.size();
        auto pct = [&](double p) -> double {
            if (v.empty()) return -1.0;
            size_t idx = (size_t)(p / 100.0 * (v.size() - 1));
            return v[idx];
        };
        std::cerr << "[dynamics] " << name
                  << ": records=" << total
                  << " converged=" << conv
                  << " rate=" << rate
                  << " unconverged=" << (total - conv)
                  << " conv_ms mean=" << mean
                  << " median=" << pct(50)
                  << " p95=" << pct(95)
                  << " p99=" << pct(99);
        if (name[0] == 'f') {
            std::cerr << " new_optimal_seen_before=" << seen_before << "/" << total;
        }
        std::cerr << std::endl;
    };

    summarize("closer", closer_total, closer_converged, closer_ms, 0);
    summarize("farther", farther_total, farther_converged, farther_ms, farther_new_opt_seen_before);

    // ---- 检测 brake 是否污染了本次测量 ----
    {
        uint64_t delivered_local = 0, braked = 0;
        if (queryBrakeCounters(broker_addr, delivered_local, braked)) {
            std::cerr << "[bench-broker] delivered_local=" << delivered_local
                      << " braked=" << braked << std::endl;
            if (braked > 0) {
                std::cerr << "[bench-broker] WARNING: brake filtered publications (braked>0). "
                          << "The dynamics benchmark measures Algorithm 1's closest-filter "
                          << "convergence; run the broker with brake_limit high (e.g. 1000) "
                          << "so the brake does not confound the convergence measurement."
                          << std::endl;
            }
        } else {
            std::cerr << "[bench-broker] stats UNAVAILABLE (cannot check for brake interference)"
                      << std::endl;
        }
    }

    // ---- 结束时的 UDP 丢包统计 ----
    bench::UdpSnmp snmp_end = bench::readUdpSnmp();
    if (snmp_start.valid && snmp_end.valid) {
        std::cerr << "[bench-net] udp_rcvbuf_errors_delta="
                  << (snmp_end.rcvbuf_errors - snmp_start.rcvbuf_errors)
                  << " udp_in_errors_delta="
                  << (snmp_end.in_errors - snmp_start.in_errors)
                  << std::endl;
    }

    return 0;
}
