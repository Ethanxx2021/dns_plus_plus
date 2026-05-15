#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>

#pragma pack(1)
struct PubSubMsg {
    uint16_t msg_type; // 1: Subscribe, 2: Publish, 3: Heartbeat(心跳)
    uint16_t topic_id;
    char payload[8];
};
#pragma pack()

int main() {
    // 1. 创建 UDP Socket
    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    
    // 2. 配置 Broker 的地址
    struct sockaddr_in broker_addr;
    memset(&broker_addr, 0, sizeof(broker_addr));
    broker_addr.sin_family = AF_INET;
    broker_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &broker_addr.sin_addr); // 目标 IP

    // 3. 构造订阅报文，订阅 Topic 8888
    char buffer[sizeof(PubSubMsg)];
    memset(buffer, 0, sizeof(buffer));
    PubSubMsg* msg = (PubSubMsg*)buffer;
    msg->msg_type = htons(1);   // Subscribe
    msg->topic_id = htons(8888);
    
    sendto(client_fd, buffer, sizeof(buffer), 0, 
           (struct sockaddr*)&broker_addr, sizeof(broker_addr));
    std::cout << "[Client] Subscribed to Topic 8888" << std::endl;

    // 4. 接收来自 Broker 的转发消息
    // 设置超时，不能一直傻等
    struct timeval tv;
    tv.tv_sec = 2;  // 2 秒超时
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char recv_buffer[1024];
    while (true) {
        memset(recv_buffer, 0, sizeof(recv_buffer));
        int n = recvfrom(client_fd, recv_buffer, sizeof(recv_buffer), 0, nullptr, nullptr);
        
        if (n > 0) {
            PubSubMsg* recv_msg = (PubSubMsg*)recv_buffer;
            uint16_t type = ntohs(recv_msg->msg_type);
            uint16_t topic = ntohs(recv_msg->topic_id);
            std::cout << "[Client] Received msg_type=" << type
                  << " topic=" << topic
                  << " payload=" << recv_msg->payload 
                  << std::endl << std::flush;
        }
        
        // 每 3 秒发送一次心跳，告诉 Broker “我还活着！”
        std::this_thread::sleep_for(std::chrono::seconds(3));
        msg->msg_type = htons(3); // Heartbeat
        sendto(client_fd, buffer, sizeof(buffer), 0, 
               (struct sockaddr*)&broker_addr, sizeof(broker_addr));
        std::cout << "[Client] Heartbeat sent..." << std::endl;
    }

    close(client_fd);
    return 0;
}