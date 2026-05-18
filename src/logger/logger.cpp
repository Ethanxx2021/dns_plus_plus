#include "logger/logger.h"
#include <iostream>

Logger::Logger() : running(true) {
    worker = std::thread(&Logger::process, this);
}

Logger::~Logger() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        running = false;
    }
    cv.notify_one();
    if (worker.joinable()) {
        worker.join();
    }
}

void Logger::pushLog(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        logs.push(msg);
    }
    cv.notify_one();
}

void Logger::process() {
    while (running) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return !logs.empty() || !running; });
        while (!logs.empty()) {
            std::cout << logs.front() << std::endl;
            logs.pop();
        }
    }
}