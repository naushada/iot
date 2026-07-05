cmake_minimum_required(VERSION 3.16.3)
project(ddns CXX)

# Top-level (own build) vs. pulled in via add_subdirectory() from the iot tree.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(DDNS_TOP_LEVEL ON)
else()
    set(DDNS_TOP_LEVEL OFF)
endif()

option(DDNS_BUILD_TESTS  "Build the ddns gtest suite"          ${DDNS_TOP_LEVEL})
option(DDNS_BUILD_DAEMON "Build the iot-ddnsd updater daemon"  OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── ddns_core: host-testable pure logic (detector, providers, SigV4, http) ──
# HTTP transport shells out to curl via ACE_Process; SigV4 uses OpenSSL crypto;
# the Cloudflare backend parses JSON with the in-tree nlohmann single header.
find_package(OpenSSL REQUIRED)

set(DDNS_JSON_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../apps/3rdparty/json/single_include)

add_library(ddns_core STATIC
    src/http_client.cpp
    src/public_ip.cpp
    src/sigv4.cpp
    src/provider_dyndns2.cpp
    src/provider_duckdns.cpp
    src/provider_cloudflare.cpp
    src/provider_route53.cpp
    src/provider_factory.cpp)
target_include_directories(ddns_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_include_directories(ddns_core PRIVATE
    ${DDNS_JSON_INC})
target_compile_options(ddns_core PRIVATE -Wall -Wextra)
target_link_libraries(ddns_core PUBLIC OpenSSL::Crypto)
add_library(ddns::core ALIAS ddns_core)

# ddns_core links ACE (ACE_Process in http_client). In the iot build ACE_ROOT is
# already known; standalone, fall back to the default install path.
if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()
target_include_directories(ddns_core PRIVATE ${ACE_ROOT}/include)

# ── iot-ddnsd daemon ────────────────────────────────────────────────────────
if(DDNS_BUILD_DAEMON)
    if(NOT TARGET datastore_client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../data-store
                         ${CMAKE_BINARY_DIR}/data-store)
    endif()

    add_executable(iot-ddnsd
        daemon/main.cpp
        daemon/ddns_client.cpp)
    target_include_directories(iot-ddnsd PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/daemon
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/../data-store/inc
        ${ACE_ROOT}/include)
    target_link_directories(iot-ddnsd PRIVATE ${ACE_ROOT}/lib)
    target_link_libraries(iot-ddnsd PRIVATE
        ddns_core datastore_client ACE pthread)
    target_compile_options(iot-ddnsd PRIVATE -Wall -Wextra)

    include(GNUInstallDirs)
    if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
        install(TARGETS iot-ddnsd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES schemas/ddns.lua
                DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)
    endif()
endif()

if(DDNS_BUILD_TESTS)
    enable_testing()
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
        add_subdirectory(test)
    endif()
endif()
