#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>

// ---------- 旧版协议 (12 字节) ----------
// 用于 Phase 0 的 Pub/Sub，保留向后兼容
#pragma pack(1)
struct PubSubMsg {
    uint16_t msg_type;   // 1: Subscribe, 2: Publish, 3: Heartbeat
    uint16_t topic_id;
    char payload[8];
};
#pragma pack()

// ---------- 新版协议 (20 字节) ----------
// Phase 1 开始使用，携带经纬度坐标
#pragma pack(1)
struct DnsPlusMsg {
    uint16_t msg_type;   // 1: Subscribe, 2: Publish, 3: Heartbeat
    uint16_t topic_id;
    float    lat;        // 纬度 (degrees)
    float    lon;        // 经度 (degrees)
    char     payload[8];
};
#pragma pack()

#endif