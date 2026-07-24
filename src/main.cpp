#include "broker/broker.h"
#include <iostream>
#include <fstream>
#include <string>

BrokerConfig parseConfig(const std::string& filename) {
    BrokerConfig cfg;
    cfg.broker_id = "default";
    cfg.listen_port = 8080;
    cfg.parent_addr = "";
    cfg.lat = 0.0f;
    cfg.lon = 0.0f;
    cfg.brake_limit = 2;
    cfg.brake_window_sec = 10;

    std::ifstream file(filename);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        
        if (key == "broker_id") cfg.broker_id = val;
        else if (key == "listen_port") cfg.listen_port = static_cast<uint16_t>(std::stoi(val));
        else if (key == "parent_addr") cfg.parent_addr = val;
        else if (key == "coords") {
            size_t comma = val.find(',');
            if (comma != std::string::npos) {
                cfg.lat = std::stof(val.substr(0, comma));
                cfg.lon = std::stof(val.substr(comma + 1));
            }
        }
        else if (key == "brake_limit") cfg.brake_limit = std::stoi(val);
        else if (key == "brake_window") cfg.brake_window_sec = std::stoi(val);
    }
    return cfg;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return 1;
    }

    BrokerConfig cfg = parseConfig(argv[1]);
    
    std::cout << "DNS++ Broker starting..." << std::endl;
    std::cout << "  ID: " << cfg.broker_id << std::endl;
    std::cout << "  Port: " << cfg.listen_port << std::endl;
    
    DnsMulticastBroker broker(cfg);
    broker.start();

    return 0;
}