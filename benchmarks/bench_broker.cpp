// benchmarks/bench_broker.cpp
//
// Automated benchmark for DNS++ Phase 1 (single-broker Proximity Routing)
//
// Measures:
//   - Recall: did subscriber receive their true closest publisher?
//   - Stretch: distance to received pub / distance to optimal pub
//   - Delivery count: how many publications each subscriber received
//
// Usage:
//   ./bench_broker <broker_ip> <broker_port> <num_pubs> <num_subs> <brake_limit> <num_trials> [seed]
//
// Output:
//   stdout: CSV (per-subscriber detail, one row per subscriber per trial)
//   stderr: Summary (per-trial aggregate)
//
// Example:
//   ./bench_broker 127.0.0.1 8080 10 50 2 30 42 > results.csv

#include "protocol/TlvMessage.h"
#include "utils/geo.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <chrono>
#include <thread>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

struct Publisher {
    int id;
    float lat;
    float lon;
};

struct Subscriber {
    int id;
    float lat;
    float lon;
    int fd = -1;

    // Ground truth
    int optimal_pub_id   = -1;
    double optimal_dist  = 0.0;

    // Received publications
    struct ReceivedPub {
        int pub_id;
        double latency_ms;
    };
    std::vector<ReceivedPub> received;
};

int main(int argc, char* argv[]) {
    if (argc < 7) {
        std::cerr << "Usage: " << argv[0]
                  << " <broker_ip> <broker_port> <num_pubs> <num_subs>"
                  << " <brake_limit> <num_trials> [seed]" << std::endl;
        return 1;
    }

    std::string broker_ip   = argv[1];
    uint16_t    broker_port = static_cast<uint16_t>(std::atoi(argv[2]));
    int         num_pubs    = std::atoi(argv[3]);
    int         num_subs    = std::atoi(argv[4]);
    int         brake_limit = std::atoi(argv[5]);
    int         num_trials  = std::atoi(argv[6]);
    uint32_t    seed        = (argc >= 8) ? static_cast<uint32_t>(std::atoi(argv[7])) : 42;

    srand(seed);

    // Broker address
    struct sockaddr_in broker_addr{};
    broker_addr.sin_family = AF_INET;
    broker_addr.sin_port   = htons(broker_port);
    inet_pton(AF_INET, broker_ip.c_str(), &broker_addr.sin_addr);

    // CSV header
    std::cout << "trial,num_pubs,num_subs,brake_limit,sub_id,sub_lat,sub_lon,"
              << "optimal_pub_id,optimal_dist,received_pub_id,received_dist,"
              << "stretch,recall,num_received" << std::endl;

    for (int t = 0; t < num_trials; t++) {
        std::string service = "bench_" + std::to_string(t);

        // --- Generate random coordinates ---
        std::vector<Publisher> pubs(num_pubs);
        for (int i = 0; i < num_pubs; i++) {
            pubs[i].id  = i;
            pubs[i].lat = -90.0f + static_cast<float>(rand()) / RAND_MAX * 180.0f;
            pubs[i].lon = -180.0f + static_cast<float>(rand()) / RAND_MAX * 360.0f;
        }

        std::vector<Subscriber> subs(num_subs);
        for (int i = 0; i < num_subs; i++) {
            subs[i].id  = i;
            subs[i].lat = -90.0f + static_cast<float>(rand()) / RAND_MAX * 180.0f;
            subs[i].lon = -180.0f + static_cast<float>(rand()) / RAND_MAX * 360.0f;
        }

        // --- Compute ground truth (closest publisher for each subscriber) ---
        for (auto& s : subs) {
            double min_dist = 1e18;
            int closest = -1;
            for (const auto& p : pubs) {
                double d = geoDistance(s.lat, s.lon, p.lat, p.lon);
                if (d < min_dist) { min_dist = d; closest = p.id; }
            }
            s.optimal_pub_id = closest;
            s.optimal_dist   = min_dist;
        }

        // --- Create subscriber sockets and send SUBSCRIBE ---
        for (auto& s : subs) {
            s.fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in addr{};
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port        = 0;
            bind(s.fd, (struct sockaddr*)&addr, sizeof(addr));

            TlvMessageBuilder builder(MsgType::SUBSCRIBE);
            builder.addServiceName(service);
            builder.addCoordinates(s.lat, s.lon);
            auto pkt = builder.build();
            sendto(s.fd, pkt.data(), pkt.size(), 0,
                   (struct sockaddr*)&broker_addr, sizeof(broker_addr));
        }

        // Wait for subscriptions to be processed
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // --- Send publications ---
        auto publish_start = std::chrono::steady_clock::now();

        for (const auto& p : pubs) {
            TlvMessageBuilder builder(MsgType::PUBLISH);
            builder.addServiceName(service);
            builder.addCoordinates(p.lat, p.lon);
            builder.setPayload(std::to_string(p.id));  // pub_id as payload
            auto pkt = builder.build();

            // Use temp socket for publishing (no response needed)
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            sendto(fd, pkt.data(), pkt.size(), 0,
                   (struct sockaddr*)&broker_addr, sizeof(broker_addr));
            close(fd);

            // Small delay between publications to let broker process
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // --- Collect responses (poll all subscriber sockets) ---
        std::vector<pollfd> pfds(subs.size());
        for (size_t i = 0; i < subs.size(); i++) {
            pfds[i].fd     = subs[i].fd;
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

        while (std::chrono::steady_clock::now() < deadline) {
            auto now = std::chrono::steady_clock::now();
            int remaining_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            if (remaining_ms <= 0) break;

            int ready = poll(pfds.data(), pfds.size(), remaining_ms);
            if (ready <= 0) break;

            for (size_t i = 0; i < pfds.size(); i++) {
                if (pfds[i].revents & POLLIN) {
                    uint8_t buf[2048];
                    int n = recv(pfds[i].fd, buf, sizeof(buf), 0);
                    if (n > 0) {
                        TlvMessage msg(buf, static_cast<size_t>(n));
                        if (msg.isValid() && msg.getMsgType() == MsgType::PUBLISH) {
                            // Extract pub_id from payload
                            std::string payload_str(
                                reinterpret_cast<const char*>(msg.getPayload()),
                                msg.getPayloadSize());
                            try {
                                int pub_id = std::stoi(payload_str);
                                auto recv_time = std::chrono::steady_clock::now();
                                double latency_ms = std::chrono::duration<double, std::milli>(
                                    recv_time - publish_start).count();
                                subs[i].received.push_back({pub_id, latency_ms});
                            } catch (...) {
                                // Ignore malformed payload
                            }
                        }
                    }
                }
            }
        }

        // --- Compute metrics and output CSV ---
        int    total_recall   = 0;
        double total_stretch  = 0.0;
        int    total_received = 0;

        for (const auto& s : subs) {
            // Find closest received publication
            double best_dist = 1e18;
            int    best_pub  = -1;

            for (const auto& rp : s.received) {
                if (rp.pub_id >= 0 && rp.pub_id < num_pubs) {
                    double d = geoDistance(s.lat, s.lon,
                                           pubs[rp.pub_id].lat, pubs[rp.pub_id].lon);
                    if (d < best_dist) { best_dist = d; best_pub = rp.pub_id; }
                }
            }

            bool   recall  = (best_pub == s.optimal_pub_id);
            double stretch = (best_pub >= 0 && s.optimal_dist > 0)
                             ? best_dist / s.optimal_dist : -1.0;

            if (recall) total_recall++;
            if (best_pub >= 0) { total_stretch += stretch; total_received++; }

            std::cout << t << ","
                      << num_pubs << "," << num_subs << "," << brake_limit << ","
                      << s.id << "," << s.lat << "," << s.lon << ","
                      << s.optimal_pub_id << "," << s.optimal_dist << ","
                      << best_pub << ","
                      << (best_pub >= 0 ? best_dist : -1.0) << ","
                      << stretch << "," << (recall ? 1 : 0) << ","
                      << s.received.size() << std::endl;
        }

        // Summary to stderr
        double avg_recall = static_cast<double>(total_recall) / subs.size();
        double avg_stretch = (total_received > 0)
                             ? total_stretch / total_received : -1.0;
        std::cerr << "Trial " << t << ": recall=" << avg_recall
                  << " avg_stretch=" << avg_stretch
                  << " delivered=" << total_received << "/" << subs.size()
                  << std::endl;

        // Close subscriber sockets
        for (auto& s : subs) {
            if (s.fd >= 0) close(s.fd);
        }

        // Brief pause between trials (let broker TTL expire old entries)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    return 0;
}