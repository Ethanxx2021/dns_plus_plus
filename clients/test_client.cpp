#include "protocol/TlvMessage.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string>

// Helper: receive one UDP packet (with timeout)
bool recvUdp(int fd, uint8_t* buf, size_t maxlen, size_t& out_len) {
    struct timeval tv;
    tv.tv_sec  = 15;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int n = recv(fd, buf, maxlen, 0);
    if (n <= 0) return false;
    out_len = static_cast<size_t>(n);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cout << "Usage:\n"
                  << "  Subscribe: " << argv[0] << " sub <ip> <port> <service_name> <lat> <lon> [query]\n"
                  << "  Publish:   " << argv[0] << " pub <ip> <port> <service_name> <lat> <lon> <payload>\n"
                  << "  Heartbeat: " << argv[0] << " beat <ip> <port> <service_name>\n"
                  << "\nExample:\n"
                  << "  " << argv[0] << " sub 127.0.0.1 9001 weather.example 51.5 -0.1 query\n"
                  << "  " << argv[0] << " pub 127.0.0.1 9001 weather.example 48.8 2.3 Paris-edge-1\n";
        return 1;
    }
    std::string mode = argv[1];
    std::string broker_ip = argv[2];
    uint16_t broker_port = static_cast<uint16_t>(std::stoi(argv[3]));
    struct sockaddr_in broker_addr{};
    broker_addr.sin_family = AF_INET;
    broker_addr.sin_port   = htons(broker_port);
    inet_pton(AF_INET, broker_ip.c_str(), &broker_addr.sin_addr);
    if (mode == "sub") {
        if (argc < 7) { std::cerr << "Need: sub <ip> <port> <name> <lat> <lon> [query]\n"; return 1; }
        std::string name = argv[4];
        float lat = std::stof(argv[5]);
        float lon = std::stof(argv[6]);
        bool query = (argc >= 8 && std::string(argv[7]) == "query");
        TlvMessageBuilder builder(MsgType::SUBSCRIBE);
        builder.addServiceName(name);
        builder.addCoordinates(lat, lon);
        if (query) builder.addFlags(MsgFlags::QUERY_MODE);
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
        std::cout << "Sent SUBSCRIBE for \"" << name << "\" @ " << lat << "," << lon
                  << (query ? " (query_mode)" : "") << std::endl;
        std::cout << "Listening for publications..." << std::endl;
        while (true) {
            uint8_t buf[2048];
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
        if (argc < 8) { std::cerr << "Need: pub <ip> <port> <name> <lat> <lon> <payload>\n"; return 1; }
        std::string name    = argv[4];
        float lat           = std::stof(argv[5]);
        float lon           = std::stof(argv[6]);
        std::string payload = argv[7];
        TlvMessageBuilder builder(MsgType::PUBLISH);
        builder.addServiceName(name);
        builder.addCoordinates(lat, lon);
        builder.setPayload(payload);
        auto pkt = builder.build();
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        sendto(fd, pkt.data(), pkt.size(), 0,
               (struct sockaddr*)&broker_addr, sizeof(broker_addr));
        close(fd);
        std::cout << "Sent PUBLISH for \"" << name << "\" @ " << lat << "," << lon
                  << " payload=\"" << payload << "\"" << std::endl;
    } else if (mode == "beat") {
        if (argc < 5) { std::cerr << "Need: beat <ip> <port> <name>\n"; return 1; }
        std::string name = argv[4];
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