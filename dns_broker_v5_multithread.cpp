#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <ctime>
#include <sstream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>

#pragma pack(1)
struct PubSubMsg {
    uint16_t msg_type;
    uint16_t topic_id;
    char payload[8];
};
#pragma pack()

// ---------- 日志线程 ----------
class Logger {
private:
    std::thread worker;
    std::queue<std::string> logs;
    std::mutex mtx;
    std::condition_variable cv;
    bool running = true;

    void process() {
        while (running) {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this] { return !logs.empty() || !running; });
            while (!logs.empty()) {
                std::cout << logs.front() << std::endl;
                logs.pop();
            }
        }
    }

public:
    Logger() {
        worker = std::thread(&Logger::process, this); // 启动后台日志线程
    }

    ~Logger() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            running = false;
        }
        cv.notify_one();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void pushLog(const std::string& msg) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            logs.push(msg);
        }
        cv.notify_one();
    }
};

// ---------- Broker 主类 ----------
class DnsMulticastBroker {
private:
    int server_fd;
    std::unordered_map<uint16_t, std::vector<struct sockaddr_in>> topic_table;
    std::unordered_map<uint16_t, time_t> topic_last_active;
    Logger logger;

    std::string clientAddrStr(const struct sockaddr_in& addr) const {
        std::stringstream ss;
        ss << inet_ntoa(addr.sin_addr) << ":" << ntohs(addr.sin_port);
        return ss.str();
    }

    void handleSubscribe(uint16_t topic, const struct sockaddr_in& client) {
        topic_table[topic].push_back(client);
        topic_last_active[topic] = time(nullptr);
        std::stringstream ss;
        ss << "[Subscribe] " << clientAddrStr(client) << " joined Topic " << topic
           << " (Total: " << topic_table[topic].size() << ")";
        logger.pushLog(ss.str());
    }

    void handlePublish(uint16_t topic, const char* buffer, int bytes) {
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

    void handleHeartbeat(uint16_t topic) {
        topic_last_active[topic] = time(nullptr);
        std::stringstream ss;
        ss << "[Heartbeat] Topic " << topic << " alive.";
        logger.pushLog(ss.str());
    }

    void cleanupExpiredTopics() {
        time_t now = time(nullptr);
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
    }

public:
    DnsMulticastBroker(uint16_t port) {
        server_fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);
        bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
        std::stringstream ss;
        ss << "[DNS++ Broker V5] Multithreaded, port " << port;
        logger.pushLog(ss.str());
    }

    ~DnsMulticastBroker() { close(server_fd); }

void start() {
    // 🌟 设置接收超时为 3 秒
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buffer[1024];
    struct sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    time_t last_cleanup = time(nullptr);

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytes = recvfrom(server_fd, buffer, sizeof(buffer), 0,
                             (struct sockaddr*)&client_addr, &client_len);

        // 如果超时（返回 -1 且 errno == EAGAIN/EWOULDBLOCK），则跳过处理
        if (bytes > 0 && bytes >= sizeof(PubSubMsg)) {
            const PubSubMsg* msg = reinterpret_cast<const PubSubMsg*>(buffer);
            uint16_t type = ntohs(msg->msg_type);
            uint16_t topic = ntohs(msg->topic_id);

            switch (type) {
                case 1: handleSubscribe(topic, client_addr); break;
                case 2: handlePublish(topic, buffer, bytes); break;
                case 3: handleHeartbeat(topic); break;
            }
        }

        // 🌟 无论有没有收到数据，都会执行清理检查
        time_t now = time(nullptr);
        if (now - last_cleanup > 10) {
            cleanupExpiredTopics();
            last_cleanup = now;
        }
    }
}
};

int main() {
    DnsMulticastBroker broker(8080);
    broker.start();
    return 0;
}