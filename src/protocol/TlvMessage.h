#ifndef TLV_MESSAGE_H
#define TLV_MESSAGE_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <optional>
#include <utility>
#include <arpa/inet.h>

// ============================================================
// DNS++ TLV Binary Protocol
// ============================================================
//
// Wire format:
//   [Fixed Header 8 bytes] [TLV fields...] [Payload...]
//
// Fixed Header:
//   uint16_t msg_type     (network byte order)
//   uint16_t num_tlvs     (network byte order)
//   uint32_t payload_len  (network byte order)
//
// Each TLV:
//   uint16_t type         (network byte order)
//   uint16_t length       (network byte order)
//   uint8_t  value[length]
//
// ============================================================

// ----- Message Types -----
enum class MsgType : uint16_t {
    SUBSCRIBE      = 0x0001,
    PUBLISH        = 0x0002,
    HEARTBEAT      = 0x0003,
    HELLO          = 0x0004,   // Phase 2: broker registration
    BROKER_FORWARD = 0x0005,   // Phase 2: broker-to-broker
    REGION_UPDATE  = 0x0006,   // Phase 2: MBH update
};

// ----- TLV Field Types -----
enum class TlvType : uint16_t {
    COORDINATES      = 0x0001,  // 8 bytes: float lat, float lon
    FLAGS            = 0x0002,  // 4 bytes: uint32_t
    SERVICE_NAME     = 0x0003,  // variable: null-terminated UTF-8 string
    BRAKE_LIMIT      = 0x0004,  // 4 bytes: int32_t
    REGION           = 0x0005,  // 16 bytes: 4x float (min_lat,max_lat,min_lon,max_lon) — Phase 2
    BLINDED_VALUE    = 0x0010,  // variable: bytes — Phase 3
    BLINDED_VALUE_HI = 0x0011,  // variable: bytes — Phase 3 (s+1)
    BLINDED_COVER    = 0x0012,  // variable: bytes — Phase 3
};

// ----- Flags bitmask -----
namespace MsgFlags {
    constexpr uint32_t NONE        = 0x00000000;
    constexpr uint32_t QUERY_MODE  = 0x00000001;  // immediate response requested
    constexpr uint32_t FROM_CHILD  = 0x00000002;  // from child broker (Phase 2)
    constexpr uint32_t FROM_PARENT = 0x00000004;  // from parent broker (Phase 2)
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
// TlvMessage — Parser (read-only, wraps a raw buffer)
// ============================================================
class TlvMessage {
public:
    // Parse from raw buffer received via recvfrom()
    TlvMessage(const uint8_t* data, size_t len);

    bool     isValid()    const { return valid_; }
    MsgType  getMsgType() const { return msg_type_; }
    uint16_t getNumTlvs() const { return num_tlvs_; }
    uint32_t getPayloadLen() const { return payload_len_; }

    // TLV accessors — return std::nullopt if field not present
    std::optional<std::pair<float, float>> getCoordinates() const;
    std::optional<uint32_t>                getFlags() const;
    std::optional<std::string>             getServiceName() const;
    std::optional<int32_t>                 getBrakeLimit() const;

    // Payload accessor
    const uint8_t* getPayload()    const;
    size_t         getPayloadSize() const;

    // Raw data (for forwarding the entire message unchanged)
    const uint8_t* getRawData() const { return data_; }
    size_t         getRawSize() const { return len_; }

private:
    const uint8_t* data_;
    size_t         len_;
    bool           valid_ = false;
    MsgType        msg_type_ = MsgType::SUBSCRIBE;
    uint16_t       num_tlvs_ = 0;
    uint32_t       payload_len_ = 0;
    size_t         payload_offset_ = 0;  // where payload starts

    // Find a TLV by type. Returns pointer to value, or nullptr.
    // If out_len is non-null, writes the value length to it.
    const uint8_t* findTlv(TlvType type, uint16_t* out_len = nullptr) const;
};

// ============================================================
// TlvMessageBuilder — Constructor (builds a wire-format buffer)
// ============================================================
class TlvMessageBuilder {
public:
    explicit TlvMessageBuilder(MsgType type);

    // Add TLV fields
    void addCoordinates(float lat, float lon);
    void addFlags(uint32_t flags);
    void addServiceName(const std::string& name);
    void addBrakeLimit(int32_t limit);

    // Set payload (the actual data, e.g. encrypted locator)
    void setPayload(const uint8_t* data, size_t len);
    void setPayload(const std::string& str);

    // Build into a vector (convenient for sendto)
    std::vector<uint8_t> build() const;

    // Build into a pre-allocated buffer. Returns bytes written, 0 on error.
    size_t build(uint8_t* buffer, size_t max_len) const;

private:
    MsgType              type_;
    std::vector<uint8_t> tlv_bytes_;   // accumulated TLV data
    std::vector<uint8_t> payload_;

    void addTlv(TlvType type, const uint8_t* data, uint16_t len);
};

#endif // TLV_MESSAGE_H