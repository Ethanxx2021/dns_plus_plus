#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <unordered_map> // 引入哈希表

// 1. 定义 Pub/Sub 协议头 (12 字节)
#pragma pack(1) // 强制 1 字节对齐，防止内存空洞
struct PubSubMsg {
    uint16_t msg_type; // 1 代表 Subscribe(订阅), 2 代表 Publish(发布)
    uint16_t topic_id; // 主题编号 (比如 8888)
    char payload[8];   // 携带的数据 (8字节)
};
#pragma pack()

class DnsBroker {
private:
    int server_fd;
    // 🌟 核心：内存路由表。记录 [主题编号 -> 订阅者的地址]
    std::unordered_map<uint16_t, struct sockaddr_in> routing_table;

public:
    DnsBroker(uint16_t port) {
        server_fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);

        bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
        std::cout << "[DNS++ Broker] Running on port " << port << std::endl;
    }

    ~DnsBroker() { close(server_fd); }

    void start() {
        char buffer[1024];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        while (true) {
            memset(buffer, 0, sizeof(buffer));
            int bytes = recvfrom(server_fd, buffer, sizeof(buffer), 0, 
                                 (struct sockaddr*)&client_addr, &client_len);
            
            if (bytes >= sizeof(PubSubMsg)) {
                // 指针强转，解析二进制报文
                PubSubMsg* msg = (PubSubMsg*)buffer;
                
                // 网络字节序转主机字节序
                uint16_t type = ntohs(msg->msg_type);
                uint16_t topic = ntohs(msg->topic_id);
                
                char* ip = inet_ntoa(client_addr.sin_addr);
                uint16_t port = ntohs(client_addr.sin_port);

                // 🌟 业务逻辑 1：处理订阅 (Subscribe)
                if (type == 1) {
                    routing_table[topic] = client_addr; // 存入路由表
                    std::cout << "[Subscribe] Client " << ip << ":" << port 
                              << " subscribed to Topic: " << topic << std::endl;
                }
                // 🌟 业务逻辑 2：处理发布 (Publish)
                else if (type == 2) {
                    std::cout << "[Publish] Received data for Topic: " << topic << std::endl;
                    
                    // 去路由表里查，有没有人订阅了这个 Topic？
                    if (routing_table.find(topic) != routing_table.end()) {
                        struct sockaddr_in subscriber = routing_table[topic];
                        
                        // 找到了！把整个报文原封不动地转发给订阅者
                        sendto(server_fd, buffer, bytes, 0, 
                               (struct sockaddr*)&subscriber, sizeof(subscriber));
                               
                        std::cout << "   -> Forwarded to subscriber " 
                                  << inet_ntoa(subscriber.sin_addr) << ":" 
                                  << ntohs(subscriber.sin_port) << std::endl;
                    } else {
                        std::cout << "   -> Dropped: No subscribers for this topic." << std::endl;
                    }
                }
            }
        }
    }
};

int main() {
    DnsBroker broker(8080);
    broker.start();
    return 0;
}