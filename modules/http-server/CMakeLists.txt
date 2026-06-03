cmake_minimum_required(VERSION 3.16.3)
project(http_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

set(HTTP_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Pull in the sibling data-store module for datastore_client.
# Skip when just building parser tests (no ACE/Lua needed for D1).
option(HTTP_SKIP_DATA_STORE "Skip data-store subdir (parser-only build)" OFF)
if(NOT HTTP_SKIP_DATA_STORE AND NOT TARGET datastore_client)
    add_subdirectory(${HTTP_MODULE_DIR}/../data-store
                     ${CMAKE_BINARY_DIR}/data-store)
endif()

include_directories(
    ${HTTP_MODULE_DIR}/inc
    ${HTTP_MODULE_DIR}/../data-store/inc
    ${HTTP_MODULE_DIR}/../../apps/3rdparty/json/single_include
    ${ACE_ROOT}/include
)

link_directories(${ACE_ROOT}/lib)

# Static lib — sources added as D-items land.
add_library(http_server_lib STATIC
    src/parser.cpp)

target_include_directories(http_server_lib PUBLIC src)
target_link_libraries(http_server_lib
    PUBLIC datastore_client ACE pthread)

# ─────────────────────────── Tests ─────────────────────────────
option(BUILD_HTTP_SERVER_TESTS "Build http-server gtest suite" ON)
if(BUILD_HTTP_SERVER_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    # D1 — parser tests are pure C++17, no ACE needed.
    # Compile parser.cpp directly to avoid ACE/Lua link deps.
    add_executable(http-parser-tests
        test/parser_test.cpp
        src/parser.cpp)

    target_include_directories(http-parser-tests
        PRIVATE ${HTTP_MODULE_DIR}/inc
        PRIVATE ${HTTP_MODULE_DIR}/src)

    target_link_libraries(http-parser-tests
        GTest::gtest_main
        GTest::gtest)

    add_test(NAME http-parser-tests COMMAND http-parser-tests)
endif()

# ─────────────────────────── Install ───────────────────────────
include(GNUInstallDirs)
if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
    install(FILES schemas/http.lua
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)
endif()
