cmake_minimum_required(VERSION 3.10)
project(dns_plus_plus CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# --- Sanitizers ---
# 之前 -DENABLE_ASAN=ON 会报 "Manually-specified variables were not used by the
# project"，因为这个选项从未被定义 —— 所谓的 ASan 构建其实是普通 Debug 构建。
# 这里用 CMAKE_CXX_FLAGS / CMAKE_EXE_LINKER_FLAGS 而不是 add_link_options()，
# 因为后者需要 CMake 3.13，而本项目声明的最低版本是 3.10。
# 必须放在所有 add_executable() 之前，才能作用于全部 target。
option(ENABLE_ASAN "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(ENABLE_ASAN)
    set(SANITIZER_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_FLAGS}")
    message(STATUS "ENABLE_ASAN=ON -> ${SANITIZER_FLAGS}")
endif()

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

# --- Unit Test: Brake Scope (spawns a real dns_broker subprocess) ---
add_executable(test_brake
    tests/test_brake.cpp
    src/protocol/TlvMessage.cpp
)
target_include_directories(test_brake PRIVATE src)
add_dependencies(test_brake dns_broker)

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

add_executable(test_crypto_microbench
    tests/test_crypto_microbench.cpp
    src/crypto/Paillier.cpp
    src/crypto/Heps.cpp
)
target_include_directories(test_crypto_microbench PRIVATE src /usr/include/x86_64-linux-gnu)
target_link_libraries(test_crypto_microbench PRIVATE gmp gmpxx)
