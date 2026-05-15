#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <ctime>  // 添加 time 头文件

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
    // 🌟 新增：记录每个 Topic 的最后活跃时间（秒级时间戳）
    std::unordered_map<uint16_t, time_t> topic_last_active;

public:
    DnsMulticastBroker(uint16_t port) {
        server_fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);
        bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
        std::cout << "[DNS++ Multicast Broker] Listening on port " << port << std::endl;
    }

    ~DnsMulticastBroker() { close(server_fd); }

    void start() {
        char buffer[1024];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        // 静态变量用于控制清理周期
        static time_t last_check = time(nullptr);

        while (true) {
            memset(buffer, 0, sizeof(buffer));
            int bytes = recvfrom(server_fd, buffer, sizeof(buffer), 0,
                                 (struct sockaddr*)&client_addr, &client_len);

            if (bytes >= sizeof(PubSubMsg)) {
                PubSubMsg* msg = (PubSubMsg*)buffer;
                uint16_t type = ntohs(msg->msg_type);
                uint16_t topic = ntohs(msg->topic_id);

                // ---------- 处理订阅 (1) ----------
                if (type == 1) {
                    topic_table[topic].push_back(client_addr);
                    topic_last_active[topic] = time(nullptr); // 记录订阅时间
                    char* ip = inet_ntoa(client_addr.sin_addr);
                    uint16_t port = ntohs(client_addr.sin_port);
                    std::cout << "[Subscribe] Client " << ip << ":" << port
                              << " joined Topic " << topic
                              << " (Total subscribers: " << topic_table[topic].size() << ")" << std::endl;
                }
                // ---------- 处理发布 (2) ----------
                else if (type == 2) {
                    std::cout << "[Publish] Data for Topic " << topic << std::endl;
                    if (topic_table.find(topic) != topic_table.end()) {
                        auto& subscribers = topic_table[topic];
                        for (const auto& sub : subscribers) {
                            sendto(server_fd, buffer, bytes, 0,
                                   (struct sockaddr*)&sub, sizeof(sub));
                        }
                        // 更新发布活跃时间
                        topic_last_active[topic] = time(nullptr);
                        std::cout << "   -> Multcast to " << subscribers.size()
                                  << " subscribers." << std::endl;
                    } else {
                        std::cout << "   -> No subscribers for this topic." << std::endl;
                    }
                }
                // ---------- 处理心跳 (3) ----------
                else if (type == 3) {
                    topic_last_active[topic] = time(nullptr);
                    std::cout << "[Heartbeat] Topic " << topic << " is alive." << std::endl;
                }

                // ---------- 定期清理过期 Topic（每 10 秒扫描一次） ----------
                if (time(nullptr) - last_check > 10) {
                    for (auto it = topic_table.begin(); it != topic_table.end(); ) {
                        // 如果超过 15 秒未活跃，则删除该 Topic
                        if (time(nullptr) - topic_last_active[it->first] > 15) {
                            std::cout << "[Cleanup] Topic " << it->first
                                      << " expired, removed." << std::endl;
                            topic_last_active.erase(it->first);  // 同时删除活跃时间记录
                            it = topic_table.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    last_check = time(nullptr);
                }
            }
        }
    }
};

int main() {
    DnsMulticastBroker broker(8080);
    broker.start();
    return 0;
}