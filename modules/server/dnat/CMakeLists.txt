cmake_minimum_required(VERSION 3.16.3)
project(server_dnat CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DNAT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

include_directories(${DNAT_DIR}/inc ${ACE_ROOT}/include)

add_library(server_dnat STATIC
    src/device_dnat.cpp)

target_include_directories(server_dnat PUBLIC inc ${ACE_ROOT}/include)
target_link_directories(server_dnat PUBLIC ${ACE_ROOT}/lib)
target_link_libraries(server_dnat PUBLIC ACE pthread)

option(BUILD_SERVER_DNAT_TESTS "Build server/dnat gtest suite" OFF)
if(BUILD_SERVER_DNAT_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    add_executable(device-dnat-tests
        test/device_dnat_test.cpp
        src/device_dnat.cpp)
    target_include_directories(device-dnat-tests
        PRIVATE ${DNAT_DIR}/inc ${ACE_ROOT}/include)
    target_link_directories(device-dnat-tests PRIVATE ${ACE_ROOT}/lib)
    target_link_libraries(device-dnat-tests
        GTest::gtest_main GTest::gtest ACE pthread)
    add_test(NAME device-dnat-tests COMMAND device-dnat-tests)
endif()
