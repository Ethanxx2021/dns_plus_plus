#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <unordered_map>
#include <vector> // 引入 vector

// 协议头定义（与昨天一致）
#pragma pack(1)
struct PubSubMsg {
    uint16_t msg_type; // 1: Subscribe, 2: Publish
    uint16_t topic_id;
    char payload[8];
};
#pragma pack()

class DnsMulticastBroker {
private:
    int server_fd;
    // 🌟 核心升级：路由表现在存储的是“订阅者列表”！
    std::unordered_map<uint16_t, std::vector<struct sockaddr_in>> topic_table;

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

        while (true) {
            memset(buffer, 0, sizeof(buffer));
            int bytes = recvfrom(server_fd, buffer, sizeof(buffer), 0, 
                                 (struct sockaddr*)&client_addr, &client_len);
            
            if (bytes >= sizeof(PubSubMsg)) {
                PubSubMsg* msg = (PubSubMsg*)buffer;
                uint16_t type = ntohs(msg->msg_type);
                uint16_t topic = ntohs(msg->topic_id);

                // 处理订阅
                if (type == 1) {
                    // 把客户地址加入对应 topic 的 vector 中
                    topic_table[topic].push_back(client_addr);
                    
                    char* ip = inet_ntoa(client_addr.sin_addr);
                    uint16_t port = ntohs(client_addr.sin_port);
                    std::cout << "[Subscribe] Client " << ip << ":" << port
                              << " joined Topic " << topic 
                              << " (Total subscribers: " << topic_table[topic].size() << ")" << std::endl;
                }
                // 处理发布
                else if (type == 2) {
                    std::cout << "[Publish] Data for Topic " << topic << std::endl;

                    // 查找是否有订阅者
                    if (topic_table.find(topic) != topic_table.end()) {
                        auto& subscribers = topic_table[topic]; // 获取订阅者列表

                        // 遍历所有订阅者，逐一转发！
                        for (const auto& sub : subscribers) {
                            sendto(server_fd, buffer, bytes, 0, 
                                   (struct sockaddr*)&sub, sizeof(sub));
                        }
                        std::cout << "   -> Multcast to " << subscribers.size() 
                                  << " subscribers." << std::endl;
                    } else {
                        std::cout << "   -> No subscribers for this topic." << std::endl;
                    }
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