cmake_minimum_required(VERSION 3.10)
project(dns_plus_plus CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# --- Broker ---
add_executable(dns_broker
    src/main.cpp
    src/broker/broker.cpp
    src/protocol/TlvMessage.cpp
    src/logger/logger.cpp
    src/crypto/Paillier.cpp
    src/crypto/Heps.cpp
)
target_include_directories(dns_broker PRIVATE src /usr/include/x86_64-linux-gnu)
target_link_libraries(dns_broker PRIVATE gmp gmpxx)

# --- Test Client ---
add_executable(test_client
    clients/test_client.cpp
    src/protocol/TlvMessage.cpp
    src/crypto/Paillier.cpp
    src/crypto/Heps.cpp
)
target_include_directories(test_client PRIVATE src /usr/include/x86_64-linux-gnu)
target_link_libraries(test_client PRIVATE gmp gmpxx)

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

# --- Benchmark ---
add_executable(bench_broker
    benchmarks/bench_broker.cpp
    src/protocol/TlvMessage.cpp
    src/crypto/Paillier.cpp
    src/crypto/Heps.cpp
)
target_include_directories(bench_broker PRIVATE src /usr/include/x86_64-linux-gnu)
target_link_libraries(bench_broker PRIVATE gmp gmpxx)

# --- Paillier Test ---
add_executable(test_paillier
    tests/test_paillier.cpp
    src/crypto/Paillier.cpp
)
target_include_directories(test_paillier PRIVATE src /usr/include/x86_64-linux-gnu)
target_link_libraries(test_paillier PRIVATE gmp gmpxx)

# --- HEPS Test ---
add_executable(test_heps
    tests/test_heps.cpp
    src/crypto/Paillier.cpp
    src/crypto/Heps.cpp
)
target_include_directories(test_heps PRIVATE src /usr/include/x86_64-linux-gnu)
target_link_libraries(test_heps PRIVATE gmp gmpxx)

# --- Multi-Broker Benchmark ---
add_executable(bench_multi_broker
    benchmarks/bench_multi_broker.cpp
    src/protocol/TlvMessage.cpp
)
target_include_directories(bench_multi_broker PRIVATE src)