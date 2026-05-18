#include "broker/broker.h"

int main() {
    DnsMulticastBroker broker(8080);
    broker.start();
    return 0;
}