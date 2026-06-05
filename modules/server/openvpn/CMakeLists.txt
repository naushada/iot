cmake_minimum_required(VERSION 3.16.3)
project(server_openvpn CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OVPN_SERVER_DIR ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${OVPN_SERVER_DIR}/inc)

add_library(server_openvpn STATIC
    src/vpn_registry.cpp)

target_include_directories(server_openvpn PUBLIC inc)
target_link_libraries(server_openvpn PUBLIC pthread)

option(BUILD_SERVER_OPENVPN_TESTS "Build server/openvpn gtest suite" OFF)
if(BUILD_SERVER_OPENVPN_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    add_executable(vpn-registry-tests
        test/vpn_registry_test.cpp
        src/vpn_registry.cpp)
    target_include_directories(vpn-registry-tests
        PRIVATE ${OVPN_SERVER_DIR}/inc)
    target_link_libraries(vpn-registry-tests
        GTest::gtest_main GTest::gtest pthread)
    add_test(NAME vpn-registry-tests COMMAND vpn-registry-tests)
endif()
