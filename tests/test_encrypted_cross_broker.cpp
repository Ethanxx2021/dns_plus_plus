// tests/test_encrypted_cross_broker.cpp
//
// 回归测试:加密模式下,跨 broker 的 publication 能正确向下转发到子 broker。
//
// 针对双重哈希缺陷(AGENT.md + docs/audit_2026-08.md Q4/Q5):子 broker 上行转发时
// SERVICE_NAME 已经是 hashServiceName(name) 的结果,父 broker 收到后不应再哈希,
// 否则 child_active_(单次哈希) 与 pub_key(双重哈希) 对不上,publication 永远
// 无法向下转发到子 broker。
//
// 拓扑:root(19000) + leaf(19001, parent=root),两者都跑加密模式(require_he=true)。
// 在 leaf 上加密订阅、在 root 上加密发布,断言:
//   1. 订阅者确实收到 publication(端到端投递成功)
//   2. root 的 STATS_DATA_EXT: forward_down > 0(确实向下转发给了 leaf)
//   3. leaf 的 STATS_DATA_EXT: match_calls > 0 且 delivered_local > 0(leaf 做了
//      同态匹配并本地投递)
//
// 用法: ./test_encrypted_cross_broker [path-to-dns_broker]

#include "protocol/TlvMessage.h"
#include "crypto/Heps.h"
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
#include <sys/stat.h>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& msg) {
    if (cond) { std::cout << "  [PASS] " << msg << std::endl; g_pass++; }
    else      { std::cout << "  [FAIL] " << msg << std::endl; g_fail++; }
}

// STATS_DATA_EXT 索引(与 broker 的 handleStatsRequest() 一致)
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

static void writeConfig(const std::string& path, const std::string& id, uint16_t port,
                        const std::string& parent, float lat, float lon, bool require_he) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { std::cerr << "cannot write config " << path << std::endl; exit(2); }
    fprintf(f, "broker_id=%s\n", id.c_str());
    fprintf(f, "listen_port=%d\n", (int)port);
    fprintf(f, "parent_addr=%s\n", parent.c_str());
    fprintf(f, "coords=%f,%f\n", lat, lon);
    fprintf(f, "brake_limit=1000\n");   // 放宽 brake,避免限流干扰断言
    fprintf(f, "brake_window=10\n");
    fprintf(f, "brake_scope=both\n");
    fprintf(f, "require_he=%s\n", require_he ? "true" : "false");
    fclose(f);
}

// 启动 broker,等它进入监听。返回 pid(失败 -1)。
static pid_t startBroker(const std::string& bin, const std::string& cfg,
                         const std::string& logfile) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (!freopen("/dev/null", "r", stdin))  { /* ignore */ }
        if (!freopen(logfile.c_str(), "w", stdout)) { /* ignore */ }
        if (!freopen(logfile.c_str(), "w", stderr)) { /* ignore */ }
        execl(bin.c_str(), bin.c_str(), cfg.c_str(), (char*)nullptr);
        _exit(127);
    }
    for (int i = 0; i < 200; i++) {
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
    std::cerr << "broker did not become ready; log " << logfile << std::endl;
    return -1;
}

static void stopBroker(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
}

int main(int argc, char* argv[]) {
    std::cout.setf(std::ios::unitbuf);
    std::string broker_bin = (argc >= 2) ? argv[1] : "./build/dns_broker";
    std::cout << "=== Encrypted Cross-Broker Routing Test ===" << std::endl;
    std::cout << "(broker binary: " << broker_bin << ")" << std::endl;

    const uint16_t ROOT_PORT = 19000;
    const uint16_t LEAF_PORT = 19001;
    const std::string root_cfg = "/tmp/dnspp_ecb_root.conf";
    const std::string leaf_cfg = "/tmp/dnspp_ecb_leaf.conf";
    const std::string root_log = "/tmp/dnspp_ecb_root.log";
    const std::string leaf_log = "/tmp/dnspp_ecb_leaf.log";
    const std::string service  = "encrypted.crossbroker.test";
    const std::string payload  = "cross-broker-encrypted-payload";

    // 先清掉可能残留的旧密钥,保证这次跑的是"本次 root 生成"的密钥。
    unlink("/tmp/dnspp_heps.key");
    unlink("/tmp/dnspp_heps_full.key");

    // ---- 1. 启动 root(生成密钥) ----
    writeConfig(root_cfg, "ecb_root", ROOT_PORT, "", 0.0f, 0.0f, false);
    pid_t root_pid = startBroker(broker_bin, root_cfg, root_log);
    check(root_pid > 0, "root broker started");

    // ---- 2. 加载 HEPS 完整密钥(供本测试侧做盲化) ----
    Heps heps;
    bool key_loaded = heps.loadState("/tmp/dnspp_heps_full.key");
    check(key_loaded, "HEPS full key loaded (root wrote /tmp/dnspp_heps_full.key)");

    // ---- 3. 启动 leaf(parent=root, require_he=true) ----
    writeConfig(leaf_cfg, "ecb_leaf", LEAF_PORT, "127.0.0.1:19000", 51.5f, -0.1f, true);
    pid_t leaf_pid = startBroker(broker_bin, leaf_cfg, leaf_log);
    check(leaf_pid > 0, "leaf broker started");
    usleep(800 * 1000);   // 让 leaf 的 HELLO 到达 root,root 注册 children_

    if (root_pid <= 0 || leaf_pid <= 0 || !key_loaded) {
        std::cout << "\n=== " << g_pass << " passed, " << g_fail << " failed ===" << std::endl;
        return (g_fail == 0) ? 0 : 1;
    }

    // ---- 4. 订阅者:加密订阅到 leaf ----
    int sub_fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in any{}; any.sin_family = AF_INET;
    any.sin_addr.s_addr = INADDR_ANY; any.sin_port = 0;
    bind(sub_fd, (sockaddr*)&any, sizeof(any));

    auto [bval_m1, bval_m2] = heps.blindSubscription(service);
    {
        TlvMessageBuilder sub(MsgType::SUBSCRIBE);
        sub.addServiceName(service);
        sub.addCoordinates(51.5f, -0.1f);
        sub.addBlindedValue(bval_m1);
        sub.addBlindedValueHi(bval_m2);
        sendPkt(sub_fd, sub.build(), makeAddr(LEAF_PORT));
    }
    usleep(800 * 1000);   // 让 leaf 把 FROM_CHILD SUBSCRIBE 上行转发给 root

    // ---- 5. 发布者:加密发布到 root ----
    {
        std::string bval_n = heps.blindNotification(service);
        TlvMessageBuilder pub(MsgType::PUBLISH);
        pub.addServiceName(service);
        pub.addCoordinates(52.2f, 13.0f);
        pub.setPayload(payload);
        pub.addBlindedValue(bval_n);

        int pub_fd = socket(AF_INET, SOCK_DGRAM, 0);
        sendPkt(pub_fd, pub.build(), makeAddr(ROOT_PORT));
        close(pub_fd);
    }

    // ---- 6. 订阅者等待接收(5s 超时) ----
    bool received = false;
    std::string recv_payload;
    {
        timeval tv = {5, 0};
        setsockopt(sub_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        uint8_t buf[4096];
        int n = recv(sub_fd, buf, sizeof(buf), 0);
        if (n > 0) {
            TlvMessage msg(buf, static_cast<size_t>(n));
            if (msg.isValid() && msg.getMsgType() == MsgType::PUBLISH &&
                msg.getPayload() && msg.getPayloadSize() > 0) {
                recv_payload.assign(reinterpret_cast<const char*>(msg.getPayload()),
                                    msg.getPayloadSize());
                received = true;
            }
        }
    }
    check(received, "subscriber received the forwarded publication");
    if (received) {
        std::cout << "    payload=\"" << recv_payload << "\"" << std::endl;
        check(recv_payload == payload, "received payload matches what was published");
    }
    close(sub_fd);

    // ---- 7. 断言 broker 侧计数器 ----
    usleep(300 * 1000);
    {
        uint64_t st[IX_COUNT] = {0};
        bool ok = queryStats(ROOT_PORT, st);
        check(ok, "root STATS_DATA_EXT received");
        if (ok) {
            std::cout << "    [root] forward_down=" << st[IX_FORWARD_DOWN]
                      << " forward_up=" << st[IX_FORWARD_UP]
                      << " delivered_local=" << st[IX_DELIVERED_LOCAL]
                      << " he_mode=" << st[IX_HE_MODE] << std::endl;
            check(st[IX_HE_MODE] == 1, "root ran in HE (encrypted) mode");
            check(st[IX_FORWARD_DOWN] > 0,
                  "root forwarded the publication downward (forward_down > 0)");
        }
    }
    {
        uint64_t st[IX_COUNT] = {0};
        bool ok = queryStats(LEAF_PORT, st);
        check(ok, "leaf STATS_DATA_EXT received");
        if (ok) {
            std::cout << "    [leaf] match_calls=" << st[IX_MATCH_CALLS]
                      << " match_hits=" << st[IX_MATCH_HITS]
                      << " delivered_local=" << st[IX_DELIVERED_LOCAL]
                      << " he_mode=" << st[IX_HE_MODE] << std::endl;
            check(st[IX_HE_MODE] == 1, "leaf ran in HE (encrypted) mode");
            check(st[IX_MATCH_CALLS] > 0, "leaf executed homomorphic Match (match_calls > 0)");
            check(st[IX_DELIVERED_LOCAL] > 0, "leaf delivered locally (delivered_local > 0)");
        }
    }

    stopBroker(leaf_pid);
    stopBroker(root_pid);

    std::cout << "\n=== " << g_pass << " passed, " << g_fail << " failed ===" << std::endl;
    return (g_fail == 0) ? 0 : 1;
}
