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

# ── iot-ddnsd daemon (device-side Dynamic DNS updater) ─────────────────────
# Built in the integrated iot image (apps/CMakeLists sets DDNS_BUILD_DAEMON and
# has already added datastore_client). Off by default for a plain
# `cmake modules/ddns` build. See apps/docs/tdd-ddns.md.
if(DDNS_BUILD_DAEMON)
    if(NOT DEFINED ACE_ROOT)
        set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
    endif()
    # data-store client (reuse the parent's target when present).
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
        datastore_client ACE pthread)
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
