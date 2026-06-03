cmake_minimum_required(VERSION 3.16.3)
project(net_router CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

set(NET_ROUTER_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Pull in the data-store client lib (and its public headers) the same
# way the openvpn/client module does. Skip the re-add when a parent
# project already added it (apps/CMakeLists.txt does in the integrated
# build).
if(NOT TARGET datastore_client)
    add_subdirectory(${NET_ROUTER_MODULE_DIR}/../../data-store
                     ${CMAKE_BINARY_DIR}/data-store)
endif()

include_directories(
    ${NET_ROUTER_MODULE_DIR}/inc
    ${NET_ROUTER_MODULE_DIR}/../../data-store/inc
    ${NET_ROUTER_MODULE_DIR}/../../../apps/3rdparty/json/single_include
    ${ACE_ROOT}/include
)

link_directories(${ACE_ROOT}/lib)

# Internal lib so test targets link the same code the binary uses.
# D2 ships only main_impl.cpp; D3..D6 append ds_bridge/nft_rules/
# ip_route/iface_monitor/apply.
add_library(net_router_lib STATIC
    src/main_impl.cpp
    src/daemon.cpp
    src/ds_bridge.cpp
    src/nft_rules.cpp
    src/shell.cpp
    src/iface_monitor.cpp
    src/ip_route.cpp
    src/apply.cpp
    src/lifecycle.cpp)

target_include_directories(net_router_lib PUBLIC src)
target_link_libraries(net_router_lib
    PUBLIC datastore_client ACE pthread)

add_executable(net-router src/main.cpp)
target_link_libraries(net-router PRIVATE net_router_lib)

# ───────────────────────── Install ─────────────────────────
include(GNUInstallDirs)
if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
    install(TARGETS net-router
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES schemas/net.lua
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)
endif()

# ───────────────────────── Tests ───────────────────────────
option(BUILD_NET_ROUTER_TESTS "Build net-router gtest suite" OFF)
if(BUILD_NET_ROUTER_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    add_executable(net-router-tests
        test/ds_bridge_test.cpp
        test/nft_rules_test.cpp
        test/iface_monitor_test.cpp
        test/ip_route_test.cpp
        test/apply_test.cpp
        test/lifecycle_test.cpp
        test/supervisor_test.cpp)

    target_link_libraries(net-router-tests
        net_router_lib
        GTest::gtest_main
        GTest::gtest)

    add_test(NAME net-router-tests COMMAND net-router-tests)
endif()
