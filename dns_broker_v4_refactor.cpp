#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <ctime>
#include <sstream>   // 引入 stringstream

#pragma pack(1)
struct PubSubMsg {
    uint16_t msg_type; // 1: Subscribe, 2: Publish, 3: Heartbeat
    uint16_t topic_id;
    char payload[8];
};
#pragma pack()

class DnsMulticastBroker {
private:
    int server_fd;
    std::unordered_map<uint16_t, std::vector<struct sockaddr_in>> topic_table;
    std::unordered_map<uint16_t, time_t> topic_last_active;

    // ---------- 私有辅助方法 ----------

    // 生成客户端地址字符串
    std::string clientAddrStr(const struct sockaddr_in& addr) const {
        std::stringstream ss;
        ss << inet_ntoa(addr.sin_addr) << ":" << ntohs(addr.sin_port);
        return ss.str();
    }

    // 处理订阅
    void handleSubscribe(uint16_t topic, const struct sockaddr_in& client) {
        topic_table[topic].push_back(client);
        topic_last_active[topic] = time(nullptr);

        std::stringstream ss;
        ss << "[Subscribe] Client " << clientAddrStr(client)
           << " joined Topic " << topic
           << " (Total: " << topic_table[topic].size() << ")";
        std::cout << ss.str() << std::endl;
    }

    // 处理发布（多播转发）
    void handlePublish(uint16_t topic, const char* buffer, int bytes) {
        std::stringstream ss;
        ss << "[Publish] Topic " << topic;

        auto it = topic_table.find(topic);
        if (it != topic_table.end()) {
            const auto& subscribers = it->second;
            for (const auto& sub : subscribers) {
                sendto(server_fd, buffer, bytes, 0,
                       (const struct sockaddr*)&sub, sizeof(sub));
            }
            topic_last_active[topic] = time(nullptr);
            ss << " -> Multicast to " << subscribers.size() << " subscribers.";
        } else {
            ss << " -> Dropped (no subscribers).";
        }
        std::cout << ss.str() << std::endl;
    }

    // 处理心跳
    void handleHeartbeat(uint16_t topic) {
        topic_last_active[topic] = time(nullptr);
        std::stringstream ss;
        ss << "[Heartbeat] Topic " << topic << " is alive.";
        std::cout << ss.str() << std::endl;
    }

    // 定期清理过期 Topic（超过 15 秒无任何消息）
    void cleanupExpiredTopics() {
        time_t now = time(nullptr);
        for (auto it = topic_table.begin(); it != topic_table.end(); ) {
            uint16_t topic = it->first;
            if (now - topic_last_active[topic] > 15) {
                std::stringstream ss;
                ss << "[Cleanup] Topic " << topic << " expired, removed.";
                std::cout << ss.str() << std::endl;
                topic_last_active.erase(topic);
                it = topic_table.erase(it);
            } else {
                ++it;
            }
        }
    }

public:
    DnsMulticastBroker(uint16_t port) {
        server_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (server_fd < 0) {
            std::cerr << "Socket creation failed!" << std::endl;
            exit(EXIT_FAILURE);
        }

        struct sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            std::cerr << "Bind failed!" << std::endl;
            close(server_fd);
            exit(EXIT_FAILURE);
        }

        std::stringstream ss;
        ss << "[DNS++ Multicast Broker] Listening on port " << port;
        std::cout << ss.str() << std::endl;
    }

    ~DnsMulticastBroker() {
        close(server_fd);
    }

    void start() {
        char buffer[1024];
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        time_t last_cleanup = time(nullptr);

        while (true) {
            memset(buffer, 0, sizeof(buffer));
            int bytes = recvfrom(server_fd, buffer, sizeof(buffer), 0,
                                 (struct sockaddr*)&client_addr, &client_len);
            if (bytes < sizeof(PubSubMsg)) continue;   // 忽略过短的数据包

            const PubSubMsg* msg = reinterpret_cast<const PubSubMsg*>(buffer);
            uint16_t type = ntohs(msg->msg_type);
            uint16_t topic = ntohs(msg->topic_id);

            switch (type) {
                case 1: handleSubscribe(topic, client_addr); break;
                case 2: handlePublish(topic, buffer, bytes); break;
                case 3: handleHeartbeat(topic); break;
                default: break;
            }

            // 每 10 秒检查一次过期 Topic
            time_t now = time(nullptr);
            if (now - last_cleanup > 10) {
                cleanupExpiredTopics();
                last_cleanup = now;
            }
        }
    }
};

int main() {
    DnsMulticastBroker broker(8080);
    broker.start();
    return 0;
}