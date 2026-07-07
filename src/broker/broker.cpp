#include "broker/broker.h"
#include <iostream>
#include <cstring>
#include <sstream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cmath>        // sqrt (可选，这里只用了平方比较)
#include <sys/epoll.h>
#include <errno.h>
#define MAX_EVENTS 10

// ---------- 构造 / 析构 ----------

DnsMulticastBroker::DnsMulticastBroker(uint16_t port) {
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
    logger.pushLog(ss.str());
}

DnsMulticastBroker::~DnsMulticastBroker() {
    close(server_fd);
}

// ---------- 工具函数 ----------

std::string DnsMulticastBroker::clientAddrStr(const struct sockaddr_in& addr) const {
    std::stringstream ss;
    ss << inet_ntoa(addr.sin_addr) << ":" << ntohs(addr.sin_port);
    return ss.str();
}

// ---------- 旧协议处理 (保持不变) ----------

void DnsMulticastBroker::handleSubscribe(uint16_t topic,
                                         const struct sockaddr_in& client) {
    topic_table[topic].push_back(client);
    topic_last_active[topic] = time(nullptr);
    std::stringstream ss;
    ss << "[Subscribe] " << clientAddrStr(client) << " joined Topic " << topic
       << " (Total: " << topic_table[topic].size() << ")";
    logger.pushLog(ss.str());
}

void DnsMulticastBroker::handlePublish(uint16_t topic,
                                       const char* buffer, int bytes) {
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
    logger.pushLog(ss.str());
}

void DnsMulticastBroker::handleHeartbeat(uint16_t topic) {
    topic_last_active[topic] = time(nullptr);
    std::stringstream ss;
    ss << "[Heartbeat] Topic " << topic << " alive.";
    logger.pushLog(ss.str());
}

void DnsMulticastBroker::cleanupExpiredTopics() {
    time_t now = time(nullptr);
    // 清理旧协议表
    for (auto it = topic_table.begin(); it != topic_table.end(); ) {
        uint16_t topic = it->first;
        if (now - topic_last_active[topic] > 15) {
            std::stringstream ss;
            ss << "[Cleanup] Topic " << topic << " expired.";
            logger.pushLog(ss.str());
            topic_last_active.erase(topic);
            it = topic_table.erase(it);
        } else {
            ++it;
        }
    }
    // 同时清理新协议表
    for (auto it = geo_subscribers.begin(); it != geo_subscribers.end(); ) {
        uint16_t topic = it->first;
        if (topic_last_active.find(topic) == topic_last_active.end() ||
            now - topic_last_active[topic] > 15) {
            std::stringstream ss;
            ss << "[Cleanup] Geo topic " << topic << " expired.";
            logger.pushLog(ss.str());
            topic_last_active.erase(topic);
            it = geo_subscribers.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------- 新协议处理 (带坐标) ----------

void DnsMulticastBroker::handleGeoSubscribe(uint16_t topic,
                                            const struct sockaddr_in& client,
                                            float lat, float lon) {
    GeoClient gc;
    gc.addr = client;
    gc.lat = lat;
    gc.lon = lon;
    geo_subscribers[topic].push_back(gc);
    topic_last_active[topic] = time(nullptr);   // 新协议也使用同一个活跃时间表

    std::stringstream ss;
    ss << "[Geo-Sub] " << clientAddrStr(client)
       << " @" << lat << "," << lon
       << " -> Topic " << topic
       << " (Total: " << geo_subscribers[topic].size() << ")";
    logger.pushLog(ss.str());
}

void DnsMulticastBroker::handleGeoPublish(uint16_t topic,
                                          const char* buffer, int bytes,
                                          float pub_lat, float pub_lon) {
    std::stringstream ss;
    ss << "[Geo-Pub] Topic " << topic << " from (" << pub_lat << "," << pub_lon << ")";

    auto it = geo_subscribers.find(topic);
    if (it == geo_subscribers.end() || it->second.empty()) {
        ss << " -> No geo-subscribers.";
        logger.pushLog(ss.str());
        return;
    }

    const auto& subs = it->second;
    const GeoClient* best = nullptr;
    float min_dist_sq = std::numeric_limits<float>::max();

    // 暴力最近邻搜索：比较距离平方
    for (const auto& gc : subs) {
        float dlat = gc.lat - pub_lat;
        float dlon = gc.lon - pub_lon;
        float dist_sq = dlat * dlat + dlon * dlon;
        if (dist_sq < min_dist_sq) {
            min_dist_sq = dist_sq;
            best = &gc;
        }
    }

    if (best) {
        sendto(server_fd, buffer, bytes, 0,
               (const struct sockaddr*)&best->addr, sizeof(best->addr));
        ss << " -> Sent to nearest " << clientAddrStr(best->addr)
           << " (dist_sq=" << min_dist_sq << ")";
        topic_last_active[topic] = time(nullptr);
    }
    logger.pushLog(ss.str());
}

// ---------- 主循环 ----------

void DnsMulticastBroker::start() {
    // 创建 epoll 实例
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        std::cerr << "epoll_create1 failed" << std::endl;
        return;
    }

    // 向 epoll 注册 server_fd，监听可读事件
    struct epoll_event ev;
    ev.events = EPOLLIN;         // 有数据可读时通知
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        std::cerr << "epoll_ctl failed" << std::endl;
        close(epoll_fd);
        return;
    }

    struct epoll_event events[MAX_EVENTS];  // MAX_EVENTS 可定义为 10
    char buffer[1024];
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    time_t last_cleanup = time(nullptr);

    while (true) {
        // 等待事件，超时 3000 毫秒（3秒）
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 3000);
        if (nfds < 0) {
            if (errno == EINTR) continue;  // 被信号中断，重试
            break;
        }

        // 处理所有就绪的事件
        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == server_fd) {
                // UDP 可读，可能有多条数据报（但 UDP 一次 epoll 通常对应一个包）
                // 循环读取直到 EAGAIN（如果设为非阻塞），这里我们保守一次读一个
                int bytes = recvfrom(server_fd, buffer, sizeof(buffer), 0,
                                     (struct sockaddr*)&client_addr, &client_len);
                if (bytes > 0) {
                    // --- 协议分发（与之前完全一样） ---
                    if (bytes == sizeof(DnsPlusMsg)) {
                        const DnsPlusMsg* geo = reinterpret_cast<const DnsPlusMsg*>(buffer);
                        uint16_t type  = ntohs(geo->msg_type);
                        uint16_t topic = ntohs(geo->topic_id);
                        switch (type) {
                            case 1: handleGeoSubscribe(topic, client_addr, geo->lat, geo->lon); break;
                            case 2: handleGeoPublish(topic, buffer, bytes, geo->lat, geo->lon); break;
                            case 3: handleHeartbeat(topic); break;
                        }
                    } else if (bytes >= sizeof(PubSubMsg)) {
                        const PubSubMsg* msg = reinterpret_cast<const PubSubMsg*>(buffer);
                        uint16_t type  = ntohs(msg->msg_type);
                        uint16_t topic = ntohs(msg->topic_id);
                        switch (type) {
                            case 1: handleSubscribe(topic, client_addr); break;
                            case 2: handlePublish(topic, buffer, bytes); break;
                            case 3: handleHeartbeat(topic); break;
                        }
                    }
                }
            }
        }

        // 无论是否有数据，超时后都会执行到这里，定期清理
        time_t now = time(nullptr);
        if (now - last_cleanup > 10) {
            cleanupExpiredTopics();
            last_cleanup = now;
        }
    }

    close(epoll_fd);
}