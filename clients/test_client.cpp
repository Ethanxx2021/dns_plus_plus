#include "protocol/TlvMessage.h"
#include "crypto/Heps.h"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string>
#include <vector>

static const char* KEY_FILE = "/tmp/dnspp_heps_full.key";

bool recvUdp(int fd, uint8_t* buf, size_t maxlen, size_t& out_len) {
    struct timeval tv;
    tv.tv_sec  = 10;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int n = recv(fd, buf, maxlen, 0);
    if (n <= 0) return false;
    out_len = static_cast<size_t>(n);
    return true;
}

int main(int argc, char* argv[]) {
    // --plaintext / --no-encrypt 可以出现在任意位置；先把它从位置参数里摘出来，
    // 这样下面的 argc 检查和 argv 下标与原来完全一致。
    bool encrypt = true;
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; i++) {
        std::string a = argv[i];
        if (i > 0 && (a == "--plaintext" || a == "--no-encrypt")) {
            encrypt = false;
            continue;
        }
        args.push_back(a);
    }
    const size_t nargs = args.size();

    if (nargs < 4) {
        std::cout << "Usage:\n"
                  << "  Subscribe: " << args[0] << " sub <ip> <port> <service_name> <lat> <lon> [query]\n"
                  << "  Publish:   " << args[0] << " pub <ip> <port> <service_name> <lat> <lon> <payload>\n"
                  << "  Heartbeat: " << args[0] << " beat <ip> <port> <service_name>\n"
                  << "\n"
                  << "  --plaintext (alias --no-encrypt): skip homomorphic blinding.\n"
                  << "      Default is encrypted, which requires " << KEY_FILE << "\n"
                  << "      (written by the root broker at startup).\n";
        return 1;
    }

    std::string mode = args[1];
    std::string broker_ip = args[2];
    uint16_t broker_port = static_cast<uint16_t>(std::stoi(args[3]));

    struct sockaddr_in broker_addr{};
    broker_addr.sin_family = AF_INET;
    broker_addr.sin_port   = htons(broker_port);
    inet_pton(AF_INET, broker_ip.c_str(), &broker_addr.sin_addr);

    // Initialize HEPS
    // 加密模式下密钥必须存在：现在直接看 Heps::loadState 的返回值 fail-fast，
    // 不再依赖前置探测。旧路径遇到密钥缺失会让 n_/mu_ 停在默认构造的 0，随后
    // blindSubscription() 在 GMP 里对 0 取模崩溃（SIGFPE，退出码 136，PR #1 实测）。
    Heps heps;
    if (encrypt && mode != "beat") {
        if (!heps.loadState(KEY_FILE)) {
            std::cerr << "FATAL: cannot load HEPS key from " << KEY_FILE << "\n"
                      << "       start the root broker first (it writes this file),\n"
                      << "       or pass --plaintext to run without blinding.\n";
            return EXIT_FAILURE;
        }
    }

    if (mode == "sub") {
        if (nargs < 7) { std::cerr << "Need: sub <ip> <port> <name> <lat> <lon> [query]\n"; return 1; }
        std::string name = args[4];
        float lat = std::stof(args[5]);
        float lon = std::stof(args[6]);
        bool query = (nargs >= 8 && args[7] == "query");

        TlvMessageBuilder builder(MsgType::SUBSCRIBE);
        builder.addServiceName(name);
        builder.addCoordinates(lat, lon);
        if (query) builder.addFlags(MsgFlags::QUERY_MODE);

        if (encrypt) {
            auto [bval_m1, bval_m2] = heps.blindSubscription(name);
            builder.addBlindedValue(bval_m1);
            builder.addBlindedValueHi(bval_m2);
        }

        auto pkt = builder.build();

        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) { std::cerr << "socket() failed\n"; return 1; }

        struct sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = 0;
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "bind() failed\n"; close(fd); return 1;
        }

        sendto(fd, pkt.data(), pkt.size(), 0,
               (struct sockaddr*)&broker_addr, sizeof(broker_addr));

        std::cout << "Sent " << (encrypt ? "ENCRYPTED" : "PLAINTEXT")
                  << " SUBSCRIBE for \"" << name << "\" @ " << lat << "," << lon
                  << (query ? " (query_mode)" : "") << std::endl;
        std::cout << "Listening for publications..." << std::endl;

        while (true) {
            uint8_t buf[4096];
            size_t len;
            if (!recvUdp(fd, buf, sizeof(buf), len)) {
                std::cout << "(timeout — no more messages)" << std::endl;
                break;
            }
            TlvMessage msg(buf, len);
            if (msg.isValid() && msg.getMsgType() == MsgType::PUBLISH) {
                auto coords = msg.getCoordinates();
                auto name   = msg.getServiceName();

                std::string payload_str;
                if (msg.getPayload() && msg.getPayloadSize() > 0) {
                    payload_str = std::string(
                        reinterpret_cast<const char*>(msg.getPayload()),
                        msg.getPayloadSize());
                }

                std::cout << "RECEIVED PUB: \""
                          << (name ? *name : "?") << "\" @ ("
                          << (coords ? std::to_string(coords->first) : "?") << ","
                          << (coords ? std::to_string(coords->second) : "?") << ")"
                          << " payload=\"" << payload_str << "\"" << std::endl;
            }
        }
        close(fd);

    } else if (mode == "pub") {
        if (nargs < 8) { std::cerr << "Need: pub <ip> <port> <name> <lat> <lon> <payload>\n"; return 1; }
        std::string name    = args[4];
        float lat           = std::stof(args[5]);
        float lon           = std::stof(args[6]);
        std::string payload = args[7];

        TlvMessageBuilder builder(MsgType::PUBLISH);
        builder.addServiceName(name);
        builder.addCoordinates(lat, lon);
        builder.setPayload(payload);

        if (encrypt) {
            std::string bval_n = heps.blindNotification(name);
            builder.addBlindedValue(bval_n);
        }

        auto pkt = builder.build();

        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        sendto(fd, pkt.data(), pkt.size(), 0,
               (struct sockaddr*)&broker_addr, sizeof(broker_addr));
        close(fd);

        std::cout << "Sent " << (encrypt ? "ENCRYPTED" : "PLAINTEXT")
                  << " PUBLISH for \"" << name << "\" @ " << lat << "," << lon
                  << " payload=\"" << payload << "\"" << std::endl;

    } else if (mode == "beat") {
        if (nargs < 5) { std::cerr << "Need: beat <ip> <port> <name>\n"; return 1; }
        std::string name = args[4];

        TlvMessageBuilder builder(MsgType::HEARTBEAT);
        builder.addServiceName(name);

        auto pkt = builder.build();

        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        sendto(fd, pkt.data(), pkt.size(), 0,
               (struct sockaddr*)&broker_addr, sizeof(broker_addr));
        close(fd);

        std::cout << "Sent HEARTBEAT for \"" << name << "\"" << std::endl;

    } else {
        std::cerr << "Unknown mode: " << mode << std::endl;
        return 1;
    }

    return 0;
}
