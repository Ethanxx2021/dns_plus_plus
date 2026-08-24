// tests/test_brake.cpp
//
// T3 回归测试：验证 brake 现在能作用于本地投递，且受 brake_scope 控制。
//
// 这不是纯函数单测——brake 的行为依赖完整的 broker（UDP + epoll + 滑动窗口 +
// STATS 上报），所以本测试 fork+exec 一个真实 dns_broker 进程，发消息，再通过
// STATS_DATA_EXT (TLV 0x0007) 读回计数器做断言。断言的是 broker 侧的
// braked_local 计数，而不是靠猜测的 recall/投递数字（后者会被 Algorithm 1 的
// 就近去重干扰）。
//
// 用法: ./test_brake [path-to-dns_broker]
//   默认 broker 路径 ./build/dns_broker（从仓库根目录运行时成立）。

#include "protocol/TlvMessage.h"
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <endian.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/types.h>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& msg) {
    if (cond) { std::cout << "  [PASS] " << msg << std::endl; g_pass++; }
    else      { std::cout << "  [FAIL] " << msg << std::endl; g_fail++; }
}

// STATS_DATA_EXT 索引（与 broker 的 handleStatsRequest() 一致）
enum {
    IX_FORWARD_UP = 0, IX_FORWARD_DOWN, IX_DELIVERED_LOCAL, IX_BRAKED,
    IX_MATCH_CALLS, IX_MATCH_HITS, IX_PUB_RECEIVED, IX_SUB_RECEIVED,
    IX_SUB_GROUPS, IX_HE_MODE, IX_BRAKED_UP, IX_BRAKED_LOCAL, IX_COUNT
};

static sockaddr_in makeAddr(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    return a;
}

static void sendPkt(int fd, const std::vector<uint8_t>& pkt, const sockaddr_in& to) {
    sendto(fd, pkt.data(), pkt.size(), 0, (const sockaddr*)&to, sizeof(to));
}

// 查询 STATS_DATA_EXT，返回是否成功；填入 out[IX_COUNT]。
static bool queryStats(uint16_t port, uint64_t out[IX_COUNT]) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    sockaddr_in broker = makeAddr(port);

    TlvMessageBuilder req(MsgType::STATS_REQUEST);
    auto pkt = req.build();
    sendPkt(fd, pkt, broker);

    timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[2048];
    int n = recv(fd, buf, sizeof(buf), 0);
    close(fd);
    if (n <= 0) return false;

    TlvMessage msg(buf, static_cast<size_t>(n));
    if (!msg.isValid() || msg.getMsgType() != MsgType::STATS_RESPONSE) return false;

    uint16_t len = 0;
    const uint8_t* v = msg.findTlv(TlvType::STATS_DATA_EXT, &len);
    if (!v || len < IX_COUNT * 8) return false;

    for (int i = 0; i < IX_COUNT; i++) {
        uint64_t be;
        std::memcpy(&be, v + i * 8, 8);
        out[i] = be64toh(be);
    }
    return true;
}

static void writeConfig(const std::string& path, const std::string& id,
                        uint16_t port, const std::string& scope) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { std::cerr << "cannot write config " << path << std::endl; exit(2); }
    fprintf(f, "broker_id=%s\n", id.c_str());
    fprintf(f, "listen_port=%d\n", (int)port);
    fprintf(f, "parent_addr=\n");        // 无 parent -> 单 broker
    fprintf(f, "coords=0.0,0.0\n");
    fprintf(f, "brake_limit=1\n");       // 窗口容量 1：第二条同象限即被拦
    fprintf(f, "brake_window=10\n");
    fprintf(f, "brake_scope=%s\n", scope.c_str());
    fclose(f);
}

// 启动 broker，等它进入监听。返回 pid（失败 exit）。
static pid_t startBroker(const std::string& bin, const std::string& cfg,
                         const std::string& logfile) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();  // 脱离父进程的会话/控制终端，避免子 broker 持有父进程的输出管道
        if (!freopen("/dev/null", "r", stdin))  { /* ignore */ }
        if (!freopen(logfile.c_str(), "w", stdout)) { /* ignore */ }
        if (!freopen(logfile.c_str(), "w", stderr)) { /* ignore */ }
        execl(bin.c_str(), bin.c_str(), cfg.c_str(), (char*)nullptr);
        _exit(127);
    }
    // 轮询日志里的 "Listening on port"
    for (int i = 0; i < 120; i++) {
        usleep(250 * 1000);
        FILE* f = fopen(logfile.c_str(), "r");
        if (f) {
            char line[512];
            bool ready = false;
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "Listening on port")) { ready = true; break; }
            }
            fclose(f);
            if (ready) return pid;
        }
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            std::cerr << "broker exited early; log " << logfile << std::endl;
            return -1;
        }
    }
    std::cerr << "broker did not become ready" << std::endl;
    return -1;
}

static void stopBroker(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
}

// 发一条明文 SUBSCRIBE（不带盲化值 -> pub_key == 服务名，走明文投递分支）。
static void sendSubscribe(int fd, const sockaddr_in& broker,
                          const std::string& name, float lat, float lon) {
    TlvMessageBuilder b(MsgType::SUBSCRIBE);
    b.addServiceName(name);
    b.addCoordinates(lat, lon);
    sendPkt(fd, b.build(), broker);
}

static void sendPublish(int fd, const sockaddr_in& broker,
                        const std::string& name, float lat, float lon,
                        const std::string& payload) {
    TlvMessageBuilder b(MsgType::PUBLISH);
    b.addServiceName(name);
    b.addCoordinates(lat, lon);
    b.setPayload(payload);
    sendPkt(fd, b.build(), broker);
}

int main(int argc, char* argv[]) {
    // 行缓冲/无缓冲：确保任何一步的输出都立刻落盘，即使后续异常退出也不丢。
    std::cout.setf(std::ios::unitbuf);
    std::string broker_bin = (argc >= 2) ? argv[1] : "./build/dns_broker";

    std::cout << "=== Brake Scope Unit Tests ===" << std::endl;
    std::cout << "(broker binary: " << broker_bin << ")" << std::endl;

    const std::string cfg_local = "/tmp/dnspp_test_brake_local.conf";
    const std::string cfg_up    = "/tmp/dnspp_test_brake_upward.conf";
    const std::string log_local = "/tmp/dnspp_test_brake_local.log";
    const std::string log_up    = "/tmp/dnspp_test_brake_upward.log";

    // 两条 publication 打在同一象限（lat>0, lon>0 -> 象限 0）的同一个点，
    // 排除 Algorithm 1 就近去重的干扰：断言只看 brake 计数器。
    const std::string service = "brake.test";
    const float sub_lat = 10.0f, sub_lon = 10.0f;
    const float pub_lat = 20.0f, pub_lon = 20.0f;

    // ---------- 场景 1：brake_scope=local, brake_limit=1 ----------
    std::cout << "\nTest 1: brake_scope=local -> second same-quadrant pub is braked_local"
              << std::endl;
    {
        const uint16_t port = 18080;
        writeConfig(cfg_local, "b_local", port, "local");
        pid_t pid = startBroker(broker_bin, cfg_local, log_local);
        if (pid < 0) { g_fail++; std::cout << "  [FAIL] broker start" << std::endl; }
        else {
            sockaddr_in broker = makeAddr(port);

            int sub_fd = socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in any{}; any.sin_family = AF_INET;
            any.sin_addr.s_addr = INADDR_ANY; any.sin_port = 0;
            bind(sub_fd, (sockaddr*)&any, sizeof(any));
            sendSubscribe(sub_fd, broker, service, sub_lat, sub_lon);
            usleep(300 * 1000);

            int pub_fd = socket(AF_INET, SOCK_DGRAM, 0);
            sendPublish(pub_fd, broker, service, pub_lat, pub_lon, "pub1");
            usleep(200 * 1000);
            sendPublish(pub_fd, broker, service, pub_lat, pub_lon, "pub2");
            usleep(300 * 1000);

            uint64_t st[IX_COUNT] = {0};
            bool ok = queryStats(port, st);
            check(ok, "STATS_DATA_EXT received (>=96 bytes / 12 fields)");
            if (ok) {
                std::cout << "    braked_local=" << st[IX_BRAKED_LOCAL]
                          << " braked_up=" << st[IX_BRAKED_UP]
                          << " delivered_local=" << st[IX_DELIVERED_LOCAL]
                          << " braked(total)=" << st[IX_BRAKED]
                          << " pub_received=" << st[IX_PUB_RECEIVED] << std::endl;
                check(st[IX_PUB_RECEIVED] == 2, "both publications reached the broker");
                check(st[IX_BRAKED_LOCAL] >= 1, "second pub braked locally (braked_local >= 1)");
                check(st[IX_BRAKED_UP] == 0, "no upward brake on a broker with no parent");
                check(st[IX_BRAKED] == st[IX_BRAKED_UP] + st[IX_BRAKED_LOCAL],
                      "legacy braked == braked_up + braked_local");
                check(st[IX_DELIVERED_LOCAL] >= 1, "first pub was delivered before the brake kicked in");
            }
            close(sub_fd);
            close(pub_fd);
            stopBroker(pid);
        }
    }

    // ---------- 场景 2：brake_scope=upward, 无 parent -> 本地不受影响 ----------
    std::cout << "\nTest 2: brake_scope=upward on a parent-less broker -> braked_local stays 0"
              << std::endl;
    {
        const uint16_t port = 18081;
        writeConfig(cfg_up, "b_upward", port, "upward");
        pid_t pid = startBroker(broker_bin, cfg_up, log_up);
        if (pid < 0) { g_fail++; std::cout << "  [FAIL] broker start" << std::endl; }
        else {
            sockaddr_in broker = makeAddr(port);

            int sub_fd = socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in any{}; any.sin_family = AF_INET;
            any.sin_addr.s_addr = INADDR_ANY; any.sin_port = 0;
            bind(sub_fd, (sockaddr*)&any, sizeof(any));
            sendSubscribe(sub_fd, broker, service, sub_lat, sub_lon);
            usleep(300 * 1000);

            int pub_fd = socket(AF_INET, SOCK_DGRAM, 0);
            // 发多条，确保若本地 brake 误开启一定会被触发
            sendPublish(pub_fd, broker, service, pub_lat, pub_lon, "pub1");
            usleep(150 * 1000);
            sendPublish(pub_fd, broker, service, pub_lat, pub_lon, "pub2");
            usleep(150 * 1000);
            sendPublish(pub_fd, broker, service, pub_lat, pub_lon, "pub3");
            usleep(300 * 1000);

            uint64_t st[IX_COUNT] = {0};
            bool ok = queryStats(port, st);
            check(ok, "STATS_DATA_EXT received");
            if (ok) {
                std::cout << "    braked_local=" << st[IX_BRAKED_LOCAL]
                          << " braked_up=" << st[IX_BRAKED_UP]
                          << " pub_received=" << st[IX_PUB_RECEIVED] << std::endl;
                check(st[IX_PUB_RECEIVED] == 3, "all three publications reached the broker");
                check(st[IX_BRAKED_LOCAL] == 0,
                      "upward scope never brakes local delivery (braked_local == 0)");
                check(st[IX_BRAKED_UP] == 0, "no parent -> no upward brake either");
            }
            close(sub_fd);
            close(pub_fd);
            stopBroker(pid);
        }
    }

    std::cout << "\n=== " << g_pass << " passed, " << g_fail << " failed ===" << std::endl;
    return (g_fail == 0) ? 0 : 1;
}
