#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

class Logger {
public:
    Logger();
    ~Logger();
    void pushLog(const std::string& msg);

private:
    void process();
    std::thread worker;
    std::queue<std::string> logs;
    std::mutex mtx;
    std::condition_variable cv;
    bool running;
};

#endif