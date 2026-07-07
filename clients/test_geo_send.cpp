#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>

// 协议头 —— 直接从 project 里复制出来的，保证和 Broker 一致
#pragma pack(1)
struct DnsPlusMsg {
    uint16_t msg_type;   // 1=Subscribe, 2=Publish, 3=Heartbeat
    uint16_t topic_id;
    float    lat;        // 纬度
    float    lon;        // 经度
    char     payload[8];
};
#pragma pack()

int main() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::cerr << "socket failed\n";
        return 1;
    }

    struct sockaddr_in broker_addr{};
    broker_addr.sin_family = AF_INET;
    broker_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &broker_addr.sin_addr);

    // 1) 发送订阅者1：伦敦 (51.5, -0.1)
    DnsPlusMsg sub1{};
    sub1.msg_type = htons(1);
    sub1.topic_id = htons(8888);
    sub1.lat = 51.5f;
    sub1.lon = -0.1f;
    strncpy(sub1.payload, "Sub1", sizeof(sub1.payload));
    sendto(fd, &sub1, sizeof(sub1), 0,
           (struct sockaddr*)&broker_addr, sizeof(broker_addr));
    std::cout << "Sent subscriber1 @ 51.5, -0.1\n";

    // 2) 发送订阅者2：巴黎 (48.8, 2.3)
    DnsPlusMsg sub2{};
    sub2.msg_type = htons(1);
    sub2.topic_id = htons(8888);
    sub2.lat = 48.8f;
    sub2.lon = 2.3f;
    strncpy(sub2.payload, "Sub2", sizeof(sub2.payload));
    sendto(fd, &sub2, sizeof(sub2), 0,
           (struct sockaddr*)&broker_addr, sizeof(broker_addr));
    std::cout << "Sent subscriber2 @ 48.8, 2.3\n";

    // 3) 发送发布者：伦敦附近 (51.3, -0.2)
    DnsPlusMsg pub{};
    pub.msg_type = htons(2);
    pub.topic_id = htons(8888);
    pub.lat = 51.3f;
    pub.lon = -0.2f;
    strncpy(pub.payload, "Hello", sizeof(pub.payload));
    sendto(fd, &pub, sizeof(pub), 0,
           (struct sockaddr*)&broker_addr, sizeof(broker_addr));
    std::cout << "Sent publisher @ 51.3, -0.2\n";

    close(fd);
    return 0;
}