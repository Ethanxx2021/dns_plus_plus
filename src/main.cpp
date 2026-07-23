#include "broker/broker.h"
#include <iostream>
#include <cstring>

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    int brake_limit = 2;
    time_t brake_window = 10;

    if (argc >= 2) port = static_cast<uint16_t>(std::atoi(argv[1]));
    if (argc >= 3) brake_limit = std::atoi(argv[2]);
    if (argc >= 4) brake_window = std::atoi(argv[3]);

    std::cout << "DNS++ Broker starting..." << std::endl;
    std::cout << "  Port: " << port << std::endl;
    std::cout << "  Brake limit: " << brake_limit << std::endl;
    std::cout << "  Brake window: " << brake_window << "s" << std::endl;

    DnsMulticastBroker broker(port, brake_limit, brake_window);
    broker.start();

    return 0;
}