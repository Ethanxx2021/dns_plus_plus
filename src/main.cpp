#include "broker/broker.h"
#include <iostream>
#include <fstream>
#include <string>
#include <stdexcept>
#include <cstdlib>

namespace {

// 配置错误一律 fail-fast（CLAUDE.md 不变量 I6：不静默降级）。
// 旧实现在打不开配置文件时会静默返回一整套默认值，导致一次配置写错的实验
// 伪装成一次配置正确的实验。
[[noreturn]] void configError(const std::string& filename, const std::string& what) {
    std::cerr << "FATAL: invalid broker config \"" << filename << "\": " << what << std::endl;
    std::cerr << "       refusing to start with default values (fail-fast, see CLAUDE.md I6)" << std::endl;
    exit(EXIT_FAILURE);
}

int parseIntField(const std::string& filename, const std::string& key, const std::string& val) {
    try {
        size_t pos = 0;
        int parsed = std::stoi(val, &pos);
        if (pos != val.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return parsed;
    } catch (const std::exception&) {
        configError(filename, key + "=\"" + val + "\" is not a valid integer");
    }
}

} // namespace

BrokerConfig parseConfig(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        configError(filename, "cannot open file");
    }

    BrokerConfig cfg;
    cfg.broker_id = "";
    cfg.listen_port = 0;
    cfg.parent_addr = "";
    cfg.lat = 0.0f;
    cfg.lon = 0.0f;
    // brake_limit / brake_window 是可选字段；下面这两个默认值会在启动日志里打印出来，
    // 所以它们是「可见的默认」而不是「静默的默认」。
    cfg.brake_limit = 2;
    cfg.brake_window_sec = 10;

    bool have_broker_id   = false;
    bool have_listen_port = false;
    bool have_coords      = false;

    std::string line;
    int lineno = 0;
    while (std::getline(file, line)) {
        lineno++;
        // 去掉 CRLF 配置文件的行尾 \r，否则下面的严格整数校验会误报
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            configError(filename, "line " + std::to_string(lineno) +
                                  ": expected key=value, got \"" + line + "\"");
        }

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        if (key == "broker_id") {
            if (val.empty()) {
                configError(filename, "line " + std::to_string(lineno) + ": broker_id must not be empty");
            }
            cfg.broker_id = val;
            have_broker_id = true;
        }
        else if (key == "listen_port") {
            int port = parseIntField(filename, key, val);
            if (port < 1 || port > 65535) {
                configError(filename, "listen_port=" + val + " is outside 1..65535");
            }
            cfg.listen_port = static_cast<uint16_t>(port);
            have_listen_port = true;
        }
        else if (key == "parent_addr") {
            // 空值合法：表示本 broker 是 root（has_parent_ == false）
            if (!val.empty() && val.find(':') == std::string::npos) {
                configError(filename, "parent_addr=\"" + val + "\" must be <ip>:<port>");
            }
            cfg.parent_addr = val;
        }
        else if (key == "coords") {
            size_t comma = val.find(',');
            if (comma == std::string::npos) {
                configError(filename, "coords=\"" + val + "\" must be <lat>,<lon>");
            }
            try {
                cfg.lat = std::stof(val.substr(0, comma));
                cfg.lon = std::stof(val.substr(comma + 1));
            } catch (const std::exception&) {
                configError(filename, "coords=\"" + val + "\" is not a valid <lat>,<lon> pair");
            }
            if (cfg.lat < -90.0f || cfg.lat > 90.0f || cfg.lon < -180.0f || cfg.lon > 180.0f) {
                configError(filename, "coords=\"" + val + "\" is outside lat[-90,90] / lon[-180,180]");
            }
            have_coords = true;
        }
        else if (key == "brake_limit") {
            int limit = parseIntField(filename, key, val);
            if (limit < 1) {
                configError(filename, "brake_limit=" + val + " must be >= 1");
            }
            cfg.brake_limit = limit;
        }
        else if (key == "brake_window") {
            int window = parseIntField(filename, key, val);
            if (window < 1) {
                configError(filename, "brake_window=" + val + " must be >= 1 (seconds)");
            }
            cfg.brake_window_sec = window;
        }
        else {
            // 未知 key 与拼错的 key 无法区分，静默忽略等于静默降级
            configError(filename, "line " + std::to_string(lineno) + ": unknown key \"" + key + "\"");
        }
    }

    if (!have_broker_id)   configError(filename, "missing required field: broker_id");
    if (!have_listen_port) configError(filename, "missing required field: listen_port");
    if (!have_coords)      configError(filename, "missing required field: coords");

    return cfg;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return 1;
    }

    BrokerConfig cfg = parseConfig(argv[1]);

    std::cout << "DNS++ Broker starting..." << std::endl;
    std::cout << "  Config: " << argv[1] << std::endl;
    std::cout << "  ID: " << cfg.broker_id << std::endl;
    std::cout << "  Port: " << cfg.listen_port << std::endl;
    std::cout << "  Coords: " << cfg.lat << "," << cfg.lon << std::endl;
    std::cout << "  Parent: " << (cfg.parent_addr.empty() ? "(none — ROOT)" : cfg.parent_addr) << std::endl;
    std::cout << "  Brake: limit=" << cfg.brake_limit
              << " window=" << cfg.brake_window_sec << "s" << std::endl;

    DnsMulticastBroker broker(cfg);
    broker.start();

    return 0;
}
