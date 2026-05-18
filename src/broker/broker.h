#ifndef BROKER_H
#define BROKER_H

#include <netinet/in.h>
#include <unordered_map>
#include <vector>
#include <ctime>
#include "protocol/protocol.h"
#include "logger/logger.h"

class DnsMulticastBroker {
public:
    DnsMulticastBroker(uint16_t port);
    ~DnsMulticastBroker();
    void start();

private:
    int server_fd;
    std::unordered_map<uint16_t, std::vector<struct sockaddr_in>> topic_table;
    std::unordered_map<uint16_t, time_t> topic_last_active;
    Logger logger;

    std::string clientAddrStr(const struct sockaddr_in& addr) const;
    void handleSubscribe(uint16_t topic, const struct sockaddr_in& client);
    void handlePublish(uint16_t topic, const char* buffer, int bytes);
    void handleHeartbeat(uint16_t topic);
    void cleanupExpiredTopics();
};

#endif