cmake_minimum_required(VERSION 3.16.3)
project(vehicle CXX)

# Top-level (own build) vs. pulled in via add_subdirectory() from the iot tree.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(VEHICLE_TOP_LEVEL ON)
else()
    set(VEHICLE_TOP_LEVEL OFF)
endif()

option(VEHICLE_BUILD_TESTS  "Build the vehicle gtest suite"          ${VEHICLE_TOP_LEVEL})
option(VEHICLE_BUILD_DAEMON "Build the iot-vehicled CAN/OBD daemon"  OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pure OBD-II (ISO 15765-4) PID encode/decode core: no ACE, no SocketCAN, no
# data-store — fully host-unit-testable. The CAN transport (raw + ISO-TP) and
# ds publishing live in the iot-vehicled daemon (next PR), gated by
# VEHICLE_BUILD_DAEMON (forced ON in the iot image build).
add_library(vehicle_core STATIC src/obd_pid.cpp)
target_include_directories(vehicle_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_options(vehicle_core PRIVATE -Wall -Wextra)
add_library(vehicle::core ALIAS vehicle_core)

# ── Install the vehicle.* schema (operator + daemon contract) ──
include(GNUInstallDirs)
if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
    install(FILES schemas/vehicle.lua
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)
endif()

# ── iot-vehicled daemon (privileged producer; reactor-driven, ACE-only) ──
# Built when VEHICLE_BUILD_DAEMON=ON (forced on in the iot image build). Off by
# default for a plain `cmake modules/vehicle` core/test build.
if(VEHICLE_BUILD_DAEMON)
    if(NOT DEFINED ACE_ROOT)
        set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
    endif()
    if(NOT TARGET datastore_client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../data-store
                         ${CMAKE_BINARY_DIR}/data-store)
    endif()

    add_executable(iot-vehicled
        daemon/main.cpp
        daemon/vehicle_client.cpp
        daemon/can_socket.cpp)
    target_include_directories(iot-vehicled PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/daemon
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/../data-store/inc
        ${ACE_ROOT}/include)
    target_link_directories(iot-vehicled PRIVATE ${ACE_ROOT}/lib)
    target_link_libraries(iot-vehicled PRIVATE
        vehicle_core datastore_client ACE pthread)

    if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
        install(TARGETS iot-vehicled RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()

if(VEHICLE_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(vehicle-tests test/obd_pid_test.cpp)
    target_link_libraries(vehicle-tests vehicle_core GTest::gtest GTest::gtest_main pthread)
    include(GoogleTest)
    if(VEHICLE_TOP_LEVEL)
        gtest_discover_tests(vehicle-tests)
    else()
        add_test(NAME vehicle-tests COMMAND vehicle-tests)
    endif()
endif()
