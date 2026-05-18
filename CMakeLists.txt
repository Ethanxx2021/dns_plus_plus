cmake_minimum_required(VERSION 3.10)
project(DNSPlusPlus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_executable(dns_broker
    src/main.cpp
    src/broker/broker.cpp
    src/logger/logger.cpp
)

target_include_directories(dns_broker PRIVATE src)
target_link_libraries(dns_broker pthread)