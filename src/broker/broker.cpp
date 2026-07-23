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
#define TTL_SECONDS 15
#define CLEANUP_INTERVAL_SEC 10
#define EPOLL_TIMEOUT_MS 3000

// ============================================================
// Construction / Destruction
// ============================================================

DnsMulticastBroker::DnsMulticastBroker(uint16_t port, int limit, time_t window)
    : server_fd(-1), brake_limit(limit), brake_window_sec(window)
{
    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd < 0) {
        std::cerr << "Socket creation failed!" << std::endl;
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Bind failed on port " << port << "!" << std::endl;
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    std::stringstream ss;
    ss << "[Broker] Listening on port " << port
       << " (brake_limit=" << brake_limit
       << ", window=" << brake_window_sec << "s)";
    logger.pushLog(ss.str());
}

DnsMulticastBroker::~DnsMulticastBroker() {
    close(server_fd);
}

// ============================================================
// Helpers
// ============================================================

std::string DnsMulticastBroker::clientAddrStr(const struct sockaddr_in& addr) const {
    std::stringstream ss;
    ss << inet_ntoa(addr.sin_addr) << ":" << ntohs(addr.sin_port);
    return ss.str();
}

int DnsMulticastBroker::getQuadrant(float lat, float lon) const {
    // Global quadrant (for Phase 1, before we have broker regions)
    if (lat >= 0.0f)
        return (lon >= 0.0f) ? 0 : 1;   // NE : NW
    else
        return (lon >= 0.0f) ? 2 : 3;   // SE : SW
}

bool DnsMulticastBroker::brakeAllows(const std::string& service,
                                      float lat, float lon) {
    int q = getQuadrant(lat, lon);
    auto& queue = brake_count[service][q];
    time_t now = time(nullptr);

    // Evict entries outside the sliding window
    while (!queue.empty() && (now - queue.front() > brake_window_sec)) {
        queue.pop();
    }

    if (static_cast<int>(queue.size()) >= brake_limit) {
        return false;  // braked
    }

    queue.push(now);
    return true;
}

// ============================================================
// SUBSCRIBE — Algorithm 1, Subscription Processing (lines 1-5)
// ============================================================
void DnsMulticastBroker::handleSubscribe(const TlvMessage& msg,
                                         const struct sockaddr_in& client)
{
    auto name = msg.getServiceName();
    if (!name) return;

    auto coords = msg.getCoordinates();
    float lat = coords ? coords->first  : 0.0f;
    float lon = coords ? coords->second : 0.0f;

    GeoClient gc;
    gc.addr = client;
    gc.lat  = lat;
    gc.lon  = lon;
    gc.cached_closest_dist = std::numeric_limits<double>::max();

    subscribers[*name].push_back(gc);
    last_active[*name] = time(nullptr);

    std::stringstream ss;
    ss << "[SUB] " << clientAddrStr(client)
       << " @" << lat << "," << lon
       << " -> \"" << *name << "\""
       << " (total: " << subscribers[*name].size() << ")";
    logger.pushLog(ss.str());

    // Algorithm 1, lines 3-5: if query_mode, return cached nearest
    auto flags = msg.getFlags();
    if (flags && (*flags & MsgFlags::QUERY_MODE)) {
        auto it = pub_cache.find(*name);
        if (it != pub_cache.end() && !it->second.empty()) {
            // Find cached publication nearest to this subscriber
            const CachedPub* best = nullptr;
            double best_dist = std::numeric_limits<double>::max();
            for (const auto& pub : it->second) {
                double d = geoDistance(lat, lon, pub.lat, pub.lon);
                if (d < best_dist) { best_dist = d; best = &pub; }
            }
            if (best) {
                sendto(server_fd, best->raw_message.data(),
                       best->raw_message.size(), 0,
                       (const struct sockaddr*)&client, sizeof(client));
                // Update subscriber's cached closest
                subscribers[*name].back().cached_closest_dist = best_dist;

                std::stringstream ss2;
                ss2 << "[SUB] query_mode -> sent cached pub to "
                    << clientAddrStr(client) << " (dist=" << best_dist << ")";
                logger.pushLog(ss2.str());
            }
        }
    }
}

// ============================================================
// PUBLISH — Algorithm 1, Publication Processing (lines 1-9)
// ============================================================
void DnsMulticastBroker::handlePublish(const TlvMessage& msg,
                                       const struct sockaddr_in& client)
{
    auto name = msg.getServiceName();
    if (!name) return;

    auto coords = msg.getCoordinates();
    float pub_lat = coords ? coords->first  : 0.0f;
    float pub_lon = coords ? coords->second : 0.0f;

    // --- Cache the publication FIRST (before subscriber check) ---
    CachedPub cp;
    cp.lat = pub_lat;
    cp.lon = pub_lon;
    cp.raw_message.assign(msg.getRawData(), msg.getRawData() + msg.getRawSize());
    pub_cache[*name].push_back(std::move(cp));

    // Limit cache size
    constexpr size_t MAX_PUB_CACHE = 10;
    if (pub_cache[*name].size() > MAX_PUB_CACHE) {
        pub_cache[*name].erase(pub_cache[*name].begin());
    }

    // --- Now check subscribers ---
    auto it = subscribers.find(*name);
    if (it == subscribers.end() || it->second.empty()) {
        std::stringstream ss;
        ss << "[PUB] \"" << *name << "\" from (" << pub_lat << "," << pub_lon
           << ") -> no subscribers (cached for query_mode).";
        logger.pushLog(ss.str());
        brake_count.erase(*name);
        return;
    }

    // --- Brake check ---
    if (!brakeAllows(*name, pub_lat, pub_lon)) {
        std::stringstream ss;
        ss << "[PUB] \"" << *name << "\" from (" << pub_lat << "," << pub_lon
           << ") BRAKED (quadrant=" << getQuadrant(pub_lat, pub_lon) << ")";
        logger.pushLog(ss.str());
        return;
    }

    // --- Forward to subscribers ---
    int delivered = 0;
    const uint8_t* raw = msg.getRawData();
    size_t raw_len = msg.getRawSize();

    for (auto& gc : it->second) {
        double dist = geoDistance(pub_lat, pub_lon, gc.lat, gc.lon);
        if (dist < gc.cached_closest_dist) {
            sendto(server_fd, raw, raw_len, 0,
                   (const struct sockaddr*)&gc.addr, sizeof(gc.addr));
            gc.cached_closest_dist = dist;
            delivered++;
        }
    }

    last_active[*name] = time(nullptr);

    std::stringstream ss;
    ss << "[PUB] \"" << *name << "\" from (" << pub_lat << "," << pub_lon
       << ") -> delivered to " << delivered << "/"
       << it->second.size() << " subscribers (brake passed)";
    logger.pushLog(ss.str());
}

// ============================================================
// HEARTBEAT
// ============================================================
void DnsMulticastBroker::handleHeartbeat(const TlvMessage& msg,
                                         const struct sockaddr_in& client)
{
    (void)client; // suppress unused parameter warning
    auto name = msg.getServiceName();
    if (!name) return;

    last_active[*name] = time(nullptr);

    std::stringstream ss;
    ss << "[BEAT] \"" << *name << "\" from " << clientAddrStr(client);
    logger.pushLog(ss.str());
}

// ============================================================
// TTL Cleanup
// ============================================================
void DnsMulticastBroker::cleanupExpired() {
    time_t now = time(nullptr);

    for (auto it = subscribers.begin(); it != subscribers.end(); ) {
        const auto& name = it->first;
        if (now - last_active[name] > TTL_SECONDS) {
            std::stringstream ss;
            ss << "[CLEAN] \"" << name << "\" expired (TTL=" << TTL_SECONDS << "s)";
            logger.pushLog(ss.str());
            last_active.erase(name);
            pub_cache.erase(name);
            brake_count.erase(name);
            it = subscribers.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================
// Main Event Loop (epoll, level-triggered)
// ============================================================
void DnsMulticastBroker::start() {
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        std::cerr << "epoll_create1 failed" << std::endl;
        return;
    }

    struct epoll_event ev;
    ev.events  = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
        std::cerr << "epoll_ctl failed" << std::endl;
        close(epoll_fd);
        return;
    }

    struct epoll_event events[MAX_EVENTS];
    uint8_t buffer[2048];
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    time_t last_cleanup = time(nullptr);

    while (true) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, EPOLL_TIMEOUT_MS);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd != server_fd) continue;

            int bytes = recvfrom(server_fd, buffer, sizeof(buffer), 0,
                                 (struct sockaddr*)&client_addr, &client_len);
            if (bytes <= 0) continue;

            // Parse as TLV
            TlvMessage msg(buffer, static_cast<size_t>(bytes));
            if (!msg.isValid()) {
                logger.pushLog("[WARN] Invalid TLV message received, ignoring.");
                continue;
            }

            switch (msg.getMsgType()) {
                case MsgType::SUBSCRIBE:
                    handleSubscribe(msg, client_addr);
                    break;
                case MsgType::PUBLISH:
                    handlePublish(msg, client_addr);
                    break;
                case MsgType::HEARTBEAT:
                    handleHeartbeat(msg, client_addr);
                    break;
                default:
                    logger.pushLog("[WARN] Unknown msg type, ignoring.");
                    break;
            }
        }

        // Periodic TTL cleanup
        time_t now = time(nullptr);
        if (now - last_cleanup > CLEANUP_INTERVAL_SEC) {
            cleanupExpired();
            last_cleanup = now;
        }
    }

    close(epoll_fd);
}