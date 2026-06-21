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
