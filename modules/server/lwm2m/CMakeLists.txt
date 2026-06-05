cmake_minimum_required(VERSION 3.16.3)
project(server_lwm2m CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

set(LWM2M_SERVER_DIR ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${LWM2M_SERVER_DIR}/inc
    ${ACE_ROOT}/include
)

link_directories(${ACE_ROOT}/lib)

# Static lib
add_library(server_lwm2m STATIC
    src/endpoint_registry.cpp)

target_include_directories(server_lwm2m PUBLIC inc)
target_link_libraries(server_lwm2m PUBLIC pthread)

# ── Tests ──────────────────────────────────────────────────────
option(BUILD_SERVER_LWM2M_TESTS "Build server/lwm2m gtest suite" OFF)
if(BUILD_SERVER_LWM2M_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    add_executable(ep-registry-tests
        test/endpoint_registry_test.cpp
        src/endpoint_registry.cpp)
    target_include_directories(ep-registry-tests
        PRIVATE ${LWM2M_SERVER_DIR}/inc)
    target_link_libraries(ep-registry-tests
        GTest::gtest_main GTest::gtest pthread)
    add_test(NAME ep-registry-tests COMMAND ep-registry-tests)
endif()
