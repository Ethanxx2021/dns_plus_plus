#ifndef BROKER_H
#define BROKER_H

#include <netinet/in.h>
#include <unordered_map>
#include <vector>
#include <ctime>
#include <limits>       // for std::numeric_limits
#include "protocol/protocol.h"
#include "logger/logger.h"

// 表示一个带坐标的客户端 (订阅者或发布者)
struct GeoClient {
    struct sockaddr_in addr;   // 网络地址
    float lat;                // 纬度
    float lon;                // 经度
    int brake_limit = 2;
};

class DnsMulticastBroker {
public:
    explicit DnsMulticastBroker(uint16_t port);
    ~DnsMulticastBroker();
    void start();

private:
    int server_fd;
    int brake_limit;
    // 对于每个 topic，记录每个象限的发布计数，用于 Brake
    std::unordered_map<uint16_t, std::unordered_map<int, int>> quadrant_count;
    // ---------- 旧协议路由表 (无坐标) ----------
    std::unordered_map<uint16_t, std::vector<struct sockaddr_in>> topic_table;
    std::unordered_map<uint16_t, time_t> topic_last_active;

    // ---------- 新协议路由表 (带坐标) ----------
    // 每个 topic 的订阅者列表 (含坐标)
    std::unordered_map<uint16_t, std::vector<GeoClient>> geo_subscribers;

    Logger logger;

    // ---------- 辅助函数 ----------
    std::string clientAddrStr(const struct sockaddr_in& addr) const;

    // 旧协议处理
    void handleSubscribe(uint16_t topic, const struct sockaddr_in& client);
    void handlePublish(uint16_t topic, const char* buffer, int bytes);
    void handleHeartbeat(uint16_t topic);
    void cleanupExpiredTopics();

    // 新协议处理 (带坐标)
    void handleGeoSubscribe(uint16_t topic, const struct sockaddr_in& client,
                            float lat, float lon);
    void handleGeoPublish(uint16_t topic, const char* buffer, int bytes,
                          float pub_lat, float pub_lon);
};

#endif