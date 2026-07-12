cmake_minimum_required(VERSION 3.16.3)
project(smsctl CXX)

# Top-level (own build) vs. pulled in via add_subdirectory() from the iot tree.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(SMSCTL_TOP_LEVEL ON)
else()
    set(SMSCTL_TOP_LEVEL OFF)
endif()

option(SMSCTL_BUILD_TESTS  "Build the smsctl gtest suite"           ${SMSCTL_TOP_LEVEL})
option(SMSCTL_BUILD_DAEMON "Build the iot-smsctld SMS control daemon" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

set(SMSCTL_JSON_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../apps/3rdparty/json/single_include)

# ── smsctl_core: pure, host-testable logic (parser, sessions, executors) ────
# No ACE, no ds, no modem — the executor talks to an injected DsSink, so the
# whole engine is unit-testable against a mock. OpenSSL only for the SHA-256
# that must match iot-httpd's password hashing.
find_package(OpenSSL REQUIRED)

add_library(smsctl_core STATIC
    src/parser.cpp
    src/session.cpp
    src/sha256.cpp
    src/executor.cpp)
target_include_directories(smsctl_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_include_directories(smsctl_core PRIVATE
    ${SMSCTL_JSON_INC})
target_compile_options(smsctl_core PRIVATE -Wall -Wextra)
target_link_libraries(smsctl_core PUBLIC OpenSSL::Crypto)
add_library(smsctl::core ALIAS smsctl_core)

# ── iot-smsctld daemon ──────────────────────────────────────────────────────
if(SMSCTL_BUILD_DAEMON)
    if(NOT TARGET datastore_client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../data-store
                         ${CMAKE_BINARY_DIR}/data-store)
    endif()

    add_executable(iot-smsctld
        daemon/main.cpp
        daemon/smsctl_client.cpp)
    target_include_directories(iot-smsctld PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/daemon
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/../data-store/inc
        ${SMSCTL_JSON_INC}
        ${ACE_ROOT}/include)
    target_link_directories(iot-smsctld PRIVATE ${ACE_ROOT}/lib)
    target_link_libraries(iot-smsctld PRIVATE
        smsctl_core datastore_client ACE pthread)
    target_compile_options(iot-smsctld PRIVATE -Wall -Wextra)

    include(GNUInstallDirs)
    if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
        install(TARGETS iot-smsctld RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES schemas/smsctl.lua
                DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)
    endif()
endif()

# ── tests ───────────────────────────────────────────────────────────────────
if(SMSCTL_BUILD_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    add_executable(smsctl-tests
        test/parser_test.cpp
        test/session_test.cpp
        test/executor_test.cpp)
    target_include_directories(smsctl-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${SMSCTL_JSON_INC})
    target_link_libraries(smsctl-tests
        smsctl_core
        GTest::gtest_main
        GTest::gtest)
    target_compile_options(smsctl-tests PRIVATE -Wall -Wextra)

    add_test(NAME smsctl-tests COMMAND smsctl-tests)
endif()
