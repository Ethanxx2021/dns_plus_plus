#include "broker/broker.h"

int main() {
    DnsMulticastBroker broker(8080, 2, 10);   // limit=2, window=10s
    broker.start();
    return 0;
}