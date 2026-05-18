#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>

#pragma pack(1)
struct PubSubMsg {
    uint16_t msg_type;   // 1: Subscribe, 2: Publish, 3: Heartbeat
    uint16_t topic_id;
    char payload[8];
};
#pragma pack()

#endif