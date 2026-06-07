cmake_minimum_required(VERSION 3.16.3)
project(server_lwm2m CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

set(LWM2M_SERVER_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# nlohmann/json single-header (vendored under apps/3rdparty).
set(JSON_INCLUDE_DIR
    ${LWM2M_SERVER_DIR}/../../../apps/3rdparty/json/single_include)

include_directories(
    ${LWM2M_SERVER_DIR}/inc
    ${JSON_INCLUDE_DIR}
    ${ACE_ROOT}/include
)

link_directories(${ACE_ROOT}/lib)

# Static lib
add_library(server_lwm2m STATIC
    src/endpoint_registry.cpp
    src/bootstrap.cpp
    src/cloud_credentials.cpp
    src/management.cpp)

target_include_directories(server_lwm2m PUBLIC inc)
target_link_libraries(server_lwm2m PUBLIC pthread server_openvpn)

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

    add_executable(bootstrap-tests
        test/bootstrap_test.cpp
        src/bootstrap.cpp
        src/endpoint_registry.cpp
        ../openvpn/src/vpn_registry.cpp)
    target_include_directories(bootstrap-tests
        PRIVATE ${LWM2M_SERVER_DIR}/inc
        PRIVATE ${LWM2M_SERVER_DIR}/../openvpn/inc)
    target_link_libraries(bootstrap-tests
        GTest::gtest_main GTest::gtest pthread)
    add_test(NAME bootstrap-tests COMMAND bootstrap-tests)

    add_executable(mgmt-tests
        test/management_test.cpp
        src/management.cpp
        src/endpoint_registry.cpp)
    target_include_directories(mgmt-tests
        PRIVATE ${LWM2M_SERVER_DIR}/inc)
    target_link_libraries(mgmt-tests
        GTest::gtest_main GTest::gtest pthread)
    add_test(NAME mgmt-tests COMMAND mgmt-tests)

    # PSK provisioning (task M) — cloud credential helpers.
    add_executable(cloud-credentials-tests
        test/cloud_credentials_test.cpp
        src/cloud_credentials.cpp)
    target_include_directories(cloud-credentials-tests
        PRIVATE ${LWM2M_SERVER_DIR}/inc ${JSON_INCLUDE_DIR})
    target_link_libraries(cloud-credentials-tests
        GTest::gtest_main GTest::gtest pthread)
    add_test(NAME cloud-credentials-tests COMMAND cloud-credentials-tests)
endif()
