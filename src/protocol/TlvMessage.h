#ifndef TLV_MESSAGE_H
#define TLV_MESSAGE_H

#include "utils/geo.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <optional>
#include <utility>
#include <arpa/inet.h>

// ----- Message Types -----
enum class MsgType : uint16_t {
    SUBSCRIBE      = 0x0001,
    PUBLISH        = 0x0002,
    HEARTBEAT      = 0x0003,
    HELLO          = 0x0004,
    HELLO_ACK      = 0x0005,
    REGION_UPDATE  = 0x0006,
    BROKER_FORWARD = 0x0007,
    STATS_REQUEST  = 0x0008,
    STATS_RESPONSE = 0x0009,
};

// ----- TLV Field Types -----
enum class TlvType : uint16_t {
    COORDINATES      = 0x0001,
    FLAGS            = 0x0002,
    SERVICE_NAME     = 0x0003,
    BRAKE_LIMIT      = 0x0004,
    REGION           = 0x0005,
    STATS_DATA       = 0x0006,
    BLINDED_VALUE    = 0x0010,
    BLINDED_VALUE_HI = 0x0011,
    BLINDED_COVER    = 0x0012,
};

// ----- Flags bitmask -----
namespace MsgFlags {
    constexpr uint32_t NONE        = 0x00000000;
    constexpr uint32_t QUERY_MODE  = 0x00000001;
    constexpr uint32_t FROM_CHILD  = 0x00000002;
    constexpr uint32_t FROM_PARENT = 0x00000004;
}

// ----- Float byte-order helpers -----
inline float htonf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    u = htonl(u);
    std::memcpy(&f, &u, 4);
    return f;
}

inline float ntohf(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    u = ntohl(u);
    std::memcpy(&f, &u, 4);
    return f;
}

// ============================================================
// TlvMessage — Parser
// ============================================================
class TlvMessage {
public:
    // For Phase 3 Encrypted Matching
    std::optional<std::string> getBlindedValue() const;      // bval_n or bval_m1
    std::optional<std::string> getBlindedValueHi() const;    // bval_m2
    TlvMessage(const uint8_t* data, size_t len);

    bool     isValid()    const { return valid_; }
    MsgType  getMsgType() const { return msg_type_; }
    uint16_t getNumTlvs() const { return num_tlvs_; }
    uint32_t getPayloadLen() const { return payload_len_; }

    std::optional<std::pair<float, float>> getCoordinates() const;
    std::optional<uint32_t>                getFlags() const;
    std::optional<std::string>             getServiceName() const;
    std::optional<int32_t>                 getBrakeLimit() const;
    std::optional<Region>                  getRegion() const;

    const uint8_t* getPayload()    const;
    size_t         getPayloadSize() const;

    const uint8_t* getRawData() const { return data_; }
    size_t         getRawSize() const { return len_; }

    // For Stats parsing in Benchmark
    const uint8_t* findTlv(TlvType type, uint16_t* out_len = nullptr) const;

private:
    const uint8_t* data_;
    size_t         len_;
    bool           valid_ = false;
    MsgType        msg_type_ = MsgType::SUBSCRIBE;
    uint16_t       num_tlvs_ = 0;
    uint32_t       payload_len_ = 0;
    size_t         payload_offset_ = 0;
};

// ============================================================
// TlvMessageBuilder — Constructor
// ============================================================
class TlvMessageBuilder {
public:
    void addBlindedValue(const std::string& hex_str);
    void addBlindedValueHi(const std::string& hex_str);
    explicit TlvMessageBuilder(MsgType type);

    void addCoordinates(float lat, float lon);
    void addFlags(uint32_t flags);
    void addServiceName(const std::string& name);
    void addBrakeLimit(int32_t limit);
    void addRegion(const Region& r);
    void addTlv(TlvType type, const uint8_t* data, uint16_t len); // Made public for Stats

    void setPayload(const uint8_t* data, size_t len);
    void setPayload(const std::string& str);

    std::vector<uint8_t> build() const;
    size_t build(uint8_t* buffer, size_t max_len) const;

private:
    MsgType              type_;
    std::vector<uint8_t> tlv_bytes_;
    std::vector<uint8_t> payload_;
};

#endif // TLV_MESSAGE_H