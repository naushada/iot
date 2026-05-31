cmake_minimum_required(VERSION 3.16.3)
project(openvpn_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ACE_TAO install prefix — same idiom as the data-store + apps cmake.
if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

set(OVPN_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Pull in the sibling data-store module so we get libdatastore_client +
# its public headers. Same pattern apps/CMakeLists.txt uses for the
# lwm2m binary. If data-store has already been added (e.g. by a parent
# project), skip the re-add to avoid the cmake "already exists" error.
if(NOT TARGET datastore_client)
    add_subdirectory(${OVPN_MODULE_DIR}/../../data-store
                     ${CMAKE_BINARY_DIR}/data-store)
endif()

include_directories(
    ${OVPN_MODULE_DIR}/inc
    ${OVPN_MODULE_DIR}/../../data-store/inc
    ${ACE_ROOT}/include
)

link_directories(${ACE_ROOT}/lib)

# Internal lib so test targets can link the same code that the
# binary uses. D3 added ds_bridge.cpp; D4 added mgmt_protocol.cpp;
# D5 added process.cpp; D6 will append client.cpp.
add_library(openvpn_client_lib STATIC
    src/main_impl.cpp
    src/ds_bridge.cpp
    src/mgmt_protocol.cpp
    src/process.cpp)

target_include_directories(openvpn_client_lib PUBLIC src)

target_link_libraries(openvpn_client_lib
    PUBLIC datastore_client ACE pthread)

add_executable(openvpn-client src/main.cpp)
target_link_libraries(openvpn-client PRIVATE openvpn_client_lib)

# ─────────────────────────── Tests ─────────────────────────────
# Light unit tests for the bits of openvpn_client that DON'T need a
# live ds-server (DsBridge construct-against-bad-socket path,
# missing_required() shape). Live ds-server smokes stay as
# log/L12/*-smoke.sh scripts run on demand.
option(BUILD_OPENVPN_CLIENT_TESTS "Build openvpn-client gtest suite" OFF)
if(BUILD_OPENVPN_CLIENT_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    add_executable(openvpn-client-tests
        test/ds_bridge_test.cpp
        test/mgmt_protocol_test.cpp
        test/process_test.cpp)

    target_link_libraries(openvpn-client-tests
        openvpn_client_lib
        GTest::gtest_main
        GTest::gtest)

    add_test(NAME openvpn-client-tests COMMAND openvpn-client-tests)
endif()

# Install rule. Skip if IOT_PACKAGING_INSTALL is OFF (set by the
# parent apps/CMakeLists.txt for dev-builds that shouldn't touch
# /etc/iot or system paths).
include(GNUInstallDirs)
if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
    install(TARGETS openvpn-client
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    # Ship the canonical vpn.* schema alongside iot.lua so ds-server
    # auto-loads it from its default /etc/iot/ds-schemas/ dir.
    install(FILES schemas/vpn.lua
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)
endif()
