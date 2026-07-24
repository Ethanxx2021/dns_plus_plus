cmake_minimum_required(VERSION 3.10)
project(dns_plus_plus CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(ENABLE_ASAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -g")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
endif()
if(ENABLE_TSAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -fno-omit-frame-pointer -g")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
endif()

# --- Broker ---
add_executable(dns_broker
    src/main.cpp
    src/broker/broker.cpp
    src/protocol/TlvMessage.cpp
    src/logger/logger.cpp
)
target_include_directories(dns_broker PRIVATE src)

# --- Test Client ---
add_executable(test_client
    clients/test_client.cpp
    src/protocol/TlvMessage.cpp
)
target_include_directories(test_client PRIVATE src)

# --- Unit Test: TLV Protocol ---
add_executable(test_tlv
    tests/test_tlv.cpp
    src/protocol/TlvMessage.cpp
)
target_include_directories(test_tlv PRIVATE src)

# --- Unit Test: Geo Functions ---
add_executable(test_geo
    tests/test_geo.cpp
)
target_include_directories(test_geo PRIVATE src)
# Debug build with sanitizers
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)


# --- Benchmark ---
add_executable(bench_broker
    benchmarks/bench_broker.cpp
    src/protocol/TlvMessage.cpp
)
target_include_directories(bench_broker PRIVATE src)

# --- Multi-Broker Benchmark ---
add_executable(bench_multi_broker
    benchmarks/bench_multi_broker.cpp
    src/protocol/TlvMessage.cpp
)
target_include_directories(bench_multi_broker PRIVATE src)