cmake_minimum_required(VERSION 3.16.3)
project(wifi_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ACE_TAO install prefix — same idiom as data-store + openvpn-client cmake.
if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

set(WIFI_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Pull in the sibling data-store module so we get libdatastore_client +
# its public headers. Same pattern openvpn-client uses. Skip the re-add
# if a parent project has already added data-store.
if(NOT TARGET datastore_client)
    add_subdirectory(${WIFI_MODULE_DIR}/../../../data-store
                     ${CMAKE_BINARY_DIR}/data-store)
endif()

include_directories(
    ${WIFI_MODULE_DIR}/inc
    ${WIFI_MODULE_DIR}/../../../data-store/inc
    ${WIFI_MODULE_DIR}/../../../../apps/3rdparty/json/single_include
    ${ACE_ROOT}/include
)

link_directories(${ACE_ROOT}/lib)

# Internal lib so test targets can link the same code the binary uses.
# D1 ships only main_impl; D3..D6 add ds_bridge, ctrl, process, lifecycle,
# supervisor as their phases land.
add_library(wifi_client_lib STATIC
    src/main_impl.cpp
    src/ds_bridge.cpp
    src/ctrl.cpp
    src/process.cpp)

target_include_directories(wifi_client_lib PUBLIC src)

target_link_libraries(wifi_client_lib
    PUBLIC datastore_client ACE pthread)

add_executable(wifi-client src/main.cpp)
target_link_libraries(wifi-client PRIVATE wifi_client_lib)

# ─────────────────────────── Tests ─────────────────────────────
# Light unit tests for the bits of wifi_client that don't need a live
# ds-server or a real wpa_supplicant. Smoke against fake-wpa.sh +
# real ds-server lives at log/L15/smoke.sh.
option(BUILD_WIFI_CLIENT_TESTS "Build wifi-client gtest suite" OFF)
if(BUILD_WIFI_CLIENT_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    add_executable(wifi-client-tests
        test/main_test.cpp
        test/schema_test.cpp
        test/ds_bridge_test.cpp
        test/ctrl_test.cpp
        test/process_test.cpp)

    target_link_libraries(wifi-client-tests
        wifi_client_lib
        GTest::gtest_main
        GTest::gtest)

    add_test(NAME wifi-client-tests COMMAND wifi-client-tests)
endif()

# Install rule. Skip if IOT_PACKAGING_INSTALL is OFF (set by parent
# apps/CMakeLists.txt for dev builds that shouldn't touch /etc/iot).
include(GNUInstallDirs)
if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
    install(TARGETS wifi-client
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    # Ship the canonical wifi.* schema alongside iot.lua/vpn.lua/net.lua
    # so ds-server auto-loads it from /etc/iot/ds-schemas/. D2 lands the
    # actual schema file; the install rule is here for symmetry from D1.
    if(EXISTS ${WIFI_MODULE_DIR}/schemas/wifi.lua)
        install(FILES schemas/wifi.lua
                DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)
    endif()
endif()
