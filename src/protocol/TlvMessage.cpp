#include "protocol/TlvMessage.h"
#include <cstring>

// ============================================================
// TlvMessage — Parser implementation
// ============================================================

TlvMessage::TlvMessage(const uint8_t* data, size_t len)
    : data_(data), len_(len)
{
    // Minimum: 8-byte fixed header
    if (len < 8) return;

    // Parse fixed header (all fields in network byte order)
    uint16_t raw_type;
    std::memcpy(&raw_type, data, 2);
    msg_type_ = static_cast<MsgType>(ntohs(raw_type));

    std::memcpy(&num_tlvs_, data + 2, 2);
    num_tlvs_ = ntohs(num_tlvs_);

    std::memcpy(&payload_len_, data + 4, 4);
    payload_len_ = ntohl(payload_len_);

    // Validate: TLV area + payload must fit in buffer
    size_t tlv_start = 8;
    size_t cursor = tlv_start;
    for (uint16_t i = 0; i < num_tlvs_; ++i) {
        if (cursor + 4 > len) return;         // TLV header doesn't fit
        uint16_t tlv_len;
        std::memcpy(&tlv_len, data + cursor + 2, 2);
        tlv_len = ntohs(tlv_len);
        cursor += 4 + tlv_len;                // header + value
        if (cursor > len) return;             // TLV value doesn't fit
    }

    payload_offset_ = cursor;
    if (payload_offset_ + payload_len_ > len) return;  // payload doesn't fit

    valid_ = true;
}

const uint8_t* TlvMessage::findTlv(TlvType type, uint16_t* out_len) const {
    if (!valid_) return nullptr;

    size_t cursor = 8;  // skip fixed header
    for (uint16_t i = 0; i < num_tlvs_; ++i) {
        uint16_t tlv_type, tlv_len;
        std::memcpy(&tlv_type, data_ + cursor, 2);
        tlv_type = ntohs(tlv_type);
        std::memcpy(&tlv_len, data_ + cursor + 2, 2);
        tlv_len = ntohs(tlv_len);

        if (static_cast<TlvType>(tlv_type) == type) {
            if (out_len) *out_len = tlv_len;
            return data_ + cursor + 4;  // pointer to value
        }
        cursor += 4 + tlv_len;
    }
    return nullptr;
}

std::optional<std::pair<float, float>> TlvMessage::getCoordinates() const {
    uint16_t len;
    const uint8_t* v = findTlv(TlvType::COORDINATES, &len);
    if (!v || len < 8) return std::nullopt;

    float lat, lon;
    std::memcpy(&lat, v, 4);
    std::memcpy(&lon, v + 4, 4);
    return std::make_pair(ntohf(lat), ntohf(lon));
}

std::optional<uint32_t> TlvMessage::getFlags() const {
    uint16_t len;
    const uint8_t* v = findTlv(TlvType::FLAGS, &len);
    if (!v || len < 4) return std::nullopt;

    uint32_t flags;
    std::memcpy(&flags, v, 4);
    return ntohl(flags);
}

std::optional<std::string> TlvMessage::getServiceName() const {
    uint16_t len;
    const uint8_t* v = findTlv(TlvType::SERVICE_NAME, &len);
    if (!v) return std::nullopt;

    // String may or may not be null-terminated; use explicit length
    return std::string(reinterpret_cast<const char*>(v), len);
}

std::optional<int32_t> TlvMessage::getBrakeLimit() const {
    uint16_t len;
    const uint8_t* v = findTlv(TlvType::BRAKE_LIMIT, &len);
    if (!v || len < 4) return std::nullopt;

    int32_t limit;
    std::memcpy(&limit, v, 4);
    return static_cast<int32_t>(ntohl(static_cast<uint32_t>(limit)));
}

const uint8_t* TlvMessage::getPayload() const {
    if (!valid_ || payload_len_ == 0) return nullptr;
    return data_ + payload_offset_;
}

size_t TlvMessage::getPayloadSize() const {
    return payload_len_;
}

// ============================================================
// TlvMessageBuilder — Constructor implementation
// ============================================================

TlvMessageBuilder::TlvMessageBuilder(MsgType type) : type_(type) {}

void TlvMessageBuilder::addTlv(TlvType type, const uint8_t* data, uint16_t len) {
    uint16_t net_type = htons(static_cast<uint16_t>(type));
    uint16_t net_len  = htons(len);

    tlv_bytes_.insert(tlv_bytes_.end(),
                      reinterpret_cast<const uint8_t*>(&net_type),
                      reinterpret_cast<const uint8_t*>(&net_type) + 2);
    tlv_bytes_.insert(tlv_bytes_.end(),
                      reinterpret_cast<const uint8_t*>(&net_len),
                      reinterpret_cast<const uint8_t*>(&net_len) + 2);
    tlv_bytes_.insert(tlv_bytes_.end(), data, data + len);
}

void TlvMessageBuilder::addCoordinates(float lat, float lon) {
    float net_lat = htonf(lat);
    float net_lon = htonf(lon);
    uint8_t buf[8];
    std::memcpy(buf, &net_lat, 4);
    std::memcpy(buf + 4, &net_lon, 4);
    addTlv(TlvType::COORDINATES, buf, 8);
}

void TlvMessageBuilder::addFlags(uint32_t flags) {
    uint32_t net_flags = htonl(flags);
    addTlv(TlvType::FLAGS, reinterpret_cast<const uint8_t*>(&net_flags), 4);
}

void TlvMessageBuilder::addServiceName(const std::string& name) {
    addTlv(TlvType::SERVICE_NAME,
           reinterpret_cast<const uint8_t*>(name.data()),
           static_cast<uint16_t>(name.size()));
}

void TlvMessageBuilder::addBrakeLimit(int32_t limit) {
    uint32_t net_limit = htonl(static_cast<uint32_t>(limit));
    addTlv(TlvType::BRAKE_LIMIT,
           reinterpret_cast<const uint8_t*>(&net_limit), 4);
}

void TlvMessageBuilder::setPayload(const uint8_t* data, size_t len) {
    payload_.assign(data, data + len);
}

void TlvMessageBuilder::setPayload(const std::string& str) {
    payload_.assign(str.begin(), str.end());
}

std::vector<uint8_t> TlvMessageBuilder::build() const {
    // Calculate total size
    size_t total = 8 + tlv_bytes_.size() + payload_.size();
    std::vector<uint8_t> buf(total);

    // Fixed header
    uint16_t net_type    = htons(static_cast<uint16_t>(type_));
    uint16_t net_num     = htons(static_cast<uint16_t>(
                          tlv_bytes_.empty() ? 0 :
                          tlv_bytes_.size() / 4));  // rough; recomputed below

    // Count TLVs properly
    uint16_t count = 0;
    size_t cursor = 0;
    while (cursor < tlv_bytes_.size()) {
        count++;
        uint16_t tlen;
        std::memcpy(&tlen, tlv_bytes_.data() + cursor + 2, 2);
        tlen = ntohs(tlen);
        cursor += 4 + tlen;
    }
    net_num = htons(count);

    uint32_t net_payload = htonl(static_cast<uint32_t>(payload_.size()));

    std::memcpy(buf.data(),     &net_type,    2);
    std::memcpy(buf.data() + 2, &net_num,     2);
    std::memcpy(buf.data() + 4, &net_payload, 4);

    // TLV fields
    std::memcpy(buf.data() + 8, tlv_bytes_.data(), tlv_bytes_.size());

    // Payload
    if (!payload_.empty()) {
        std::memcpy(buf.data() + 8 + tlv_bytes_.size(),
                    payload_.data(), payload_.size());
    }

    return buf;
}

size_t TlvMessageBuilder::build(uint8_t* buffer, size_t max_len) const {
    auto vec = build();
    if (vec.size() > max_len) return 0;
    std::memcpy(buffer, vec.data(), vec.size());
    return vec.size();
}