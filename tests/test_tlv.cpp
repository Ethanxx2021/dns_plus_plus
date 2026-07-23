// tests/test_tlv.cpp — Unit tests for TLV protocol serialization
#include "protocol/TlvMessage.h"
#include <iostream>
#include <cmath>
#include <string>

static int tests_passed = 0;
static int tests_failed = 0;

void check(bool cond, const std::string& name, const std::string& detail = "") {
    if (cond) {
        std::cout << "  [PASS] " << name << std::endl;
        tests_passed++;
    } else {
        std::cout << "  [FAIL] " << name;
        if (!detail.empty()) std::cout << " — " << detail;
        std::cout << std::endl;
        tests_failed++;
    }
}

int main() {
    std::cout << "=== TLV Protocol Unit Tests ===" << std::endl;

    // --- Test 1: Round-trip with all TLV fields ---
    std::cout << "Test 1: Round-trip with coordinates, name, flags" << std::endl;
    {
        TlvMessageBuilder builder(MsgType::SUBSCRIBE);
        builder.addServiceName("weather.example.com");
        builder.addCoordinates(51.5074f, -0.1278f);
        builder.addFlags(MsgFlags::QUERY_MODE);

        auto buf = builder.build();
        TlvMessage msg(buf.data(), buf.size());

        check(msg.isValid(), "Message valid");
        check(msg.getMsgType() == MsgType::SUBSCRIBE, "MsgType == SUBSCRIBE");

        auto name = msg.getServiceName();
        check(name.has_value(), "Service name present");
        check(name && *name == "weather.example.com", "Service name correct", *name);

        auto coords = msg.getCoordinates();
        check(coords.has_value(), "Coordinates present");
        check(coords && std::abs(coords->first - 51.5074f) < 0.001f, "Latitude correct");
        check(coords && std::abs(coords->second - (-0.1278f)) < 0.001f, "Longitude correct");

        auto flags = msg.getFlags();
        check(flags.has_value(), "Flags present");
        check(flags && *flags == MsgFlags::QUERY_MODE, "Flags == QUERY_MODE");
    }

    // --- Test 2: Round-trip with payload ---
    std::cout << "Test 2: Round-trip with payload" << std::endl;
    {
        TlvMessageBuilder builder(MsgType::PUBLISH);
        builder.addServiceName("cdn.edge");
        builder.addCoordinates(40.7128f, -74.0060f);
        builder.setPayload("edge-node-1");

        auto buf = builder.build();
        TlvMessage msg(buf.data(), buf.size());

        check(msg.isValid(), "Message valid");
        check(msg.getPayloadSize() == 11, "Payload size == 11");

        std::string payload_str(reinterpret_cast<const char*>(msg.getPayload()),
                                msg.getPayloadSize());
        check(payload_str == "edge-node-1", "Payload content correct", payload_str);
    }

    // --- Test 3: Round-trip with no TLVs (payload only) ---
    std::cout << "Test 3: Message with no TLV fields" << std::endl;
    {
        TlvMessageBuilder builder(MsgType::HEARTBEAT);
        builder.setPayload("alive");

        auto buf = builder.build();
        TlvMessage msg(buf.data(), buf.size());

        check(msg.isValid(), "Message valid");
        check(msg.getMsgType() == MsgType::HEARTBEAT, "MsgType == HEARTBEAT");
        check(!msg.getCoordinates().has_value(), "No coordinates (nullopt)");
        check(!msg.getServiceName().has_value(), "No service name (nullopt)");
        check(msg.getPayloadSize() == 5, "Payload size == 5");
    }

    // --- Test 4: Empty message (no TLVs, no payload) ---
    std::cout << "Test 4: Empty message" << std::endl;
    {
        TlvMessageBuilder builder(MsgType::HEARTBEAT);
        auto buf = builder.build();
        TlvMessage msg(buf.data(), buf.size());

        check(msg.isValid(), "Message valid");
        check(msg.getNumTlvs() == 0, "Zero TLVs");
        check(msg.getPayloadSize() == 0, "Zero payload");
    }

    // --- Test 5: Invalid message (too short) ---
    std::cout << "Test 5: Invalid message (buffer too short)" << std::endl;
    {
        uint8_t tiny[] = {0x00, 0x01, 0x00};
        TlvMessage msg(tiny, sizeof(tiny));
        check(!msg.isValid(), "Invalid (too short)");
    }

    // --- Test 6: Float byte-order correctness ---
    std::cout << "Test 6: Float byte-order correctness" << std::endl;
    {
        float test_vals[] = {0.0f, 1.0f, -1.0f, 51.5074f, -0.1278f, 3.14159f};
        bool all_ok = true;
        for (float v : test_vals) {
            float net = htonf(v);
            float back = ntohf(net);
            if (std::abs(back - v) > 0.0001f) {
                all_ok = false;
                std::cerr << "    Mismatch: " << v << " -> " << back << std::endl;
            }
        }
        check(all_ok, "All float round-trips correct");
    }

    // --- Test 7: Multiple TLVs of same type (last one wins) ---
    // --- Test 7: Multiple TLVs of same type (first one wins) ---
    std::cout << "Test 7: Multiple coordinates (first wins)" << std::endl;
    {
        TlvMessageBuilder builder(MsgType::SUBSCRIBE);
        builder.addCoordinates(10.0f, 20.0f);
        builder.addCoordinates(30.0f, 40.0f);  // second is ignored by findTlv
        builder.addServiceName("test");

        auto buf = builder.build();
        TlvMessage msg(buf.data(), buf.size());

        auto coords = msg.getCoordinates();
        check(coords.has_value(), "Coordinates present");
        check(coords && std::abs(coords->first - 10.0f) < 0.001f, "Latitude == 10.0 (first)");
        check(coords && std::abs(coords->second - 20.0f) < 0.001f, "Longitude == 20.0 (first)");
    }

    // --- Test 8: Long service name ---
    std::cout << "Test 8: Long service name (100 chars)" << std::endl;
    {
        std::string long_name(100, 'x');
        TlvMessageBuilder builder(MsgType::SUBSCRIBE);
        builder.addServiceName(long_name);
        builder.addCoordinates(0.0f, 0.0f);

        auto buf = builder.build();
        TlvMessage msg(buf.data(), buf.size());

        auto name = msg.getServiceName();
        check(name.has_value(), "Long name present");
        check(name && name->size() == 100, "Long name length == 100");
        check(name && *name == long_name, "Long name content correct");
    }

    // --- Summary ---
    std::cout << "\n=== " << tests_passed << " passed, " << tests_failed
              << " failed ===" << std::endl;
    return tests_failed > 0 ? 1 : 0;
}