#include "broker/broker.h"
#include <iostream>
#include <cstring>
#include <sstream>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cmath>        // sqrt (可选，这里只用了平方比较)

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
    char buffer[1024];
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    time_t last_cleanup = time(nullptr);

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytes = recvfrom(server_fd, buffer, sizeof(buffer), 0,
                             (struct sockaddr*)&client_addr, &client_len);

        // ---------- 协议识别与分发 ----------
        if (bytes == sizeof(DnsPlusMsg)) {
            // 新协议 (20 字节)
            const DnsPlusMsg* geo = reinterpret_cast<const DnsPlusMsg*>(buffer);
            uint16_t type  = ntohs(geo->msg_type);
            uint16_t topic = ntohs(geo->topic_id);
            float lat = geo->lat;
            float lon = geo->lon;

            switch (type) {
                case 1: handleGeoSubscribe(topic, client_addr, lat, lon); break;
                case 2: handleGeoPublish(topic, buffer, bytes, lat, lon); break;
                case 3: handleHeartbeat(topic); break;
            }
        }
        else if (bytes >= sizeof(PubSubMsg)) {
            // 旧协议 (≥12 字节)
            const PubSubMsg* msg = reinterpret_cast<const PubSubMsg*>(buffer);
            uint16_t type  = ntohs(msg->msg_type);
            uint16_t topic = ntohs(msg->topic_id);

            switch (type) {
                case 1: handleSubscribe(topic, client_addr); break;
                case 2: handlePublish(topic, buffer, bytes); break;
                case 3: handleHeartbeat(topic); break;
            }
        }
        // 忽略长度不足的包

        // ---------- 定期清理 ----------
        time_t now = time(nullptr);
        if (now - last_cleanup > 10) {
            cleanupExpiredTopics();
            last_cleanup = now;
        }
    }
}