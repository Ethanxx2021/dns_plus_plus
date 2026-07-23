#ifndef BROKER_H
#define BROKER_H

#include <queue>
#include <netinet/in.h>
#include <unordered_map>
#include <vector>
#include <ctime>
#include <limits>
#include <optional>
#include <string>

#include "protocol/TlvMessage.h"
#include "logger/logger.h"
#include "utils/geo.h"

// A subscriber with coordinates and per-subscriber closest cache.
// This implements Algorithm 1's closest[S] state.
struct GeoClient {
    struct sockaddr_in addr;
    float lat;
    float lon;
    // Algorithm 1, line 2/8: closest[N_in] / closest[S]
    // Initialized to infinity — first publication always qualifies.
    double cached_closest_dist = std::numeric_limits<double>::max();
};

// A cached publication (for query_mode immediate response)
struct CachedPub {
    float lat;
    float lon;
    std::vector<uint8_t> raw_message;  // original TLV message bytes
};

class DnsMulticastBroker {
public:
    explicit DnsMulticastBroker(uint16_t port,
                               int brake_limit = 2,
                               time_t brake_window_sec = 10);
    ~DnsMulticastBroker();
    void start();

private:
    int server_fd;
    int brake_limit;
    time_t brake_window_sec;

    Logger logger;

    // ---------- Routing Tables ----------
    // Key is service_name (string, per paper §3.3)
    // IT[] — Input Table: subscribers per service name
    std::unordered_map<std::string, std::vector<GeoClient>> subscribers;

    // Cached publications per service name (for query_mode)
    std::unordered_map<std::string, std::vector<CachedPub>> pub_cache;

    // TTL tracking per service name
    std::unordered_map<std::string, time_t> last_active;

    // Brake: per service_name, per quadrant, sliding window of timestamps
    std::unordered_map<std::string,
                       std::unordered_map<int, std::queue<time_t>>> brake_count;

    // ---------- Helpers ----------
    std::string clientAddrStr(const struct sockaddr_in& addr) const;
    int getQuadrant(float lat, float lon) const;

    // Check if a publication passes the brake
    bool brakeAllows(const std::string& service, float lat, float lon);

    // ---------- Message Handlers ----------
    void handleSubscribe(const TlvMessage& msg,
                         const struct sockaddr_in& client);
    void handlePublish(const TlvMessage& msg,
                       const struct sockaddr_in& client);
    void handleHeartbeat(const TlvMessage& msg,
                         const struct sockaddr_in& client);

    // ---------- Periodic Maintenance ----------
    void cleanupExpired();
};

#endif // BROKER_H