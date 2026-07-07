#include "broker/broker.h"
#include <iostream>
#include <cstring>
#include <sstream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <errno.h>
#include <cmath>
#define MAX_EVENTS 10

// ---------- 构造函数 ----------
DnsMulticastBroker::DnsMulticastBroker(uint16_t port, int limit, time_t window_sec)
    : server_fd(-1), brake_limit(limit), brake_window_sec(window_sec)

{
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
    ss << "[DNS++ Multicast Broker] Listening on port " << port
       << " (brake_limit=" << brake_limit << ", window=" << brake_window_sec << "s)";
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

// ---------- 象限判断 ----------
int DnsMulticastBroker::getQuadrant(float lat, float lon) const {
    if (lat >= 0.0f)
        return (lon >= 0.0f) ? 0 : 1;
    else
        return (lon >= 0.0f) ? 2 : 3;
}

// ---------- 旧协议处理 ----------
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

// ---------- 新协议处理（带坐标 + Brake）----------
void DnsMulticastBroker::handleGeoSubscribe(uint16_t topic,
                                            const struct sockaddr_in& client,
                                            float lat, float lon) {
    GeoClient gc;
    gc.addr = client;
    gc.lat = lat;
    gc.lon = lon;
    geo_subscribers[topic].push_back(gc);
    topic_last_active[topic] = time(nullptr);

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
    // 1. 检查是否有订阅者
    auto it = geo_subscribers.find(topic);
    if (it == geo_subscribers.end() || it->second.empty()) {
        std::stringstream ss;
        ss << "[Geo-Pub] Topic " << topic << " from (" << pub_lat << "," << pub_lon
           << ") -> No geo-subscribers.";
        logger.pushLog(ss.str());
        quadrant_count.erase(topic);   // 清理残留计数
        return;
    }

    // 2. 滑动窗口 Brake 检查
    int q = getQuadrant(pub_lat, pub_lon);
    auto& q_queue = quadrant_count[topic][q];
    time_t now = time(nullptr);
    // 移除窗口外的时间戳
    while (!q_queue.empty() && (now - q_queue.front() > brake_window_sec)) {
        q_queue.pop();
    }
    if (q_queue.size() >= static_cast<size_t>(brake_limit)) {
        std::stringstream ss;
        ss << "[Geo-Pub] Topic " << topic << " from (" << pub_lat << "," << pub_lon
           << ") quadrant " << q << " BRAKED (window count=" << q_queue.size() << ")";
        logger.pushLog(ss.str());
        return;
    }
    q_queue.push(now);   // 记录本次发布

    // 3. 最近订阅者查找与转发
    const auto& subs = it->second;
    const GeoClient* best = nullptr;
    float min_dist_sq = std::numeric_limits<float>::max();

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
        std::stringstream ss;
        ss << "[Geo-Pub] Topic " << topic << " from (" << pub_lat << "," << pub_lon
           << ") -> Sent to nearest " << clientAddrStr(best->addr)
           << " (dist_sq=" << min_dist_sq << ")";
        logger.pushLog(ss.str());
        topic_last_active[topic] = time(nullptr);
    }
}

// ---------- 定期清理 ----------
void DnsMulticastBroker::cleanupExpiredTopics() {
    time_t now = time(nullptr);

    // 旧协议多播表清理
    for (auto it = topic_table.begin(); it != topic_table.end(); ) {
        uint16_t topic = it->first;
        if (now - topic_last_active[topic] > 15) {
            std::stringstream ss;
            ss << "[Cleanup] Topic " << topic << " expired (old protocol).";
            logger.pushLog(ss.str());
            topic_last_active.erase(topic);
            it = topic_table.erase(it);
        } else {
            ++it;
        }
    }

    // 新协议 geo 表清理
    for (auto it = geo_subscribers.begin(); it != geo_subscribers.end(); ) {
        uint16_t topic = it->first;
        if (now - topic_last_active[topic] > 15) {
            std::stringstream ss;
            ss << "[Cleanup] Geo topic " << topic << " expired.";
            logger.pushLog(ss.str());
            topic_last_active.erase(topic);
            quadrant_count.erase(topic);   // 同时清除 Brake 计数
            it = geo_subscribers.erase(it);
        } else {
            ++it;
        }
    }
}

// ---------- epoll 主循环 ----------
void DnsMulticastBroker::start() {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        std::cerr << "epoll_create1 failed" << std::endl;
        return;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        std::cerr << "epoll_ctl failed" << std::endl;
        close(epoll_fd);
        return;
    }

    struct epoll_event events[MAX_EVENTS];
    char buffer[1024];
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    time_t last_cleanup = time(nullptr);

    while (true) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 3000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == server_fd) {
                int bytes = recvfrom(server_fd, buffer, sizeof(buffer), 0,
                                     (struct sockaddr*)&client_addr, &client_len);
                if (bytes > 0) {
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

        time_t now = time(nullptr);
        if (now - last_cleanup > 10) {
            cleanupExpiredTopics();
            last_cleanup = now;
        }
    }

    close(epoll_fd);
}