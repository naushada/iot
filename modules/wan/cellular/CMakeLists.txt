cmake_minimum_required(VERSION 3.16.3)
project(cellular CXX)

# Top-level (own build) vs. pulled in via add_subdirectory() from the iot tree.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CELLULAR_TOP_LEVEL ON)
else()
    set(CELLULAR_TOP_LEVEL OFF)
endif()

option(CELLULAR_BUILD_TESTS  "Build the cellular gtest suite"            ${CELLULAR_TOP_LEVEL})
option(CELLULAR_BUILD_DAEMON "Build the cellular-client modem daemon"    OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pure parsing/state core: no ACE, no data-store — fully host-unit-testable.
# The serial/AT modem control + ds publishing live in the daemon (PR-C).
add_library(cellular_core STATIC
    src/at_parser.cpp
    src/nmea_parser.cpp
    src/cell_state.cpp)
target_include_directories(cellular_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_options(cellular_core PRIVATE -Wall -Wextra)
add_library(cellular::core ALIAS cellular_core)

# ── Install the cell.* / gps.* schema (operator + daemon contract) ──
include(GNUInstallDirs)
if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
    install(FILES schemas/cell.lua
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)
endif()

if(CELLULAR_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(cellular-tests
        test/at_parser_test.cpp
        test/nmea_parser_test.cpp
        test/cell_state_test.cpp)
    target_link_libraries(cellular-tests cellular_core GTest::gtest GTest::gtest_main pthread)
    include(GoogleTest)
    if(CELLULAR_TOP_LEVEL)
        gtest_discover_tests(cellular-tests)
    else()
        add_test(NAME cellular-tests COMMAND cellular-tests)
    endif()
endif()
