cmake_minimum_required(VERSION 3.16.3)
project(http_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

set(HTTP_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# OpenSSL — native TLS termination (https) in the session layer.
find_package(OpenSSL REQUIRED)

# Pull in data-store for datastore_client (needed by handlers + main).
option(HTTP_SKIP_DATA_STORE "Skip data-store subdir (parser-only build)" OFF)
if(NOT HTTP_SKIP_DATA_STORE AND NOT TARGET datastore_client)
    add_subdirectory(${HTTP_MODULE_DIR}/../data-store
                     ${CMAKE_BINARY_DIR}/data-store)
endif()

# nlohmann::json — vendored under apps/3rdparty/json/
set(NLOHMANN_JSON_INCLUDE
    ${HTTP_MODULE_DIR}/../../apps/3rdparty/json/single_include)

include_directories(
    ${HTTP_MODULE_DIR}/inc
    ${HTTP_MODULE_DIR}/../data-store/inc
    ${NLOHMANN_JSON_INCLUDE}
    ${ACE_ROOT}/include
)

link_directories(${ACE_ROOT}/lib)

# Static lib — sources added as D-items land.
add_library(http_server_lib STATIC
    src/parser.cpp
    src/router.cpp
    src/session.cpp
    src/handler.cpp
    src/auth.cpp
    src/tls.cpp
    src/worker.cpp)

target_include_directories(http_server_lib PUBLIC src)
target_link_libraries(http_server_lib
    PUBLIC datastore_client ACE pthread OpenSSL::SSL OpenSSL::Crypto)

# ─────────────────────────── Binary ────────────────────────────
add_executable(iot-httpd src/main.cpp)
target_link_libraries(iot-httpd PRIVATE http_server_lib)

# ─────────────────────────── Tests ─────────────────────────────
# OFF by default (like the sibling modules) so a packaging/yocto build
# doesn't pull in GTest. The Dockerfile + standalone dev builds pass
# -DBUILD_HTTP_SERVER_TESTS=ON explicitly; the recipe's gtest
# PACKAGECONFIG flips it on too.
option(BUILD_HTTP_SERVER_TESTS "Build http-server gtest suite" OFF)
if(BUILD_HTTP_SERVER_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    # D1 — parser tests: pure C++17, no ACE needed.
    add_executable(http-parser-tests
        test/parser_test.cpp
        src/parser.cpp)
    target_include_directories(http-parser-tests
        PRIVATE ${HTTP_MODULE_DIR}/inc
        PRIVATE ${HTTP_MODULE_DIR}/src)
    target_link_libraries(http-parser-tests
        GTest::gtest_main GTest::gtest)
    add_test(NAME http-parser-tests COMMAND http-parser-tests)

    # D2 — router tests: pure C++17, no ACE needed.
    add_executable(http-router-tests
        test/router_test.cpp
        src/router.cpp)
    target_include_directories(http-router-tests
        PRIVATE ${HTTP_MODULE_DIR}/inc
        PRIVATE ${HTTP_MODULE_DIR}/src)
    target_link_libraries(http-router-tests
        GTest::gtest_main GTest::gtest)
    add_test(NAME http-router-tests COMMAND http-router-tests)

    # TLS tests: OpenSSL only, no ACE — a full handshake + echo between a
    # real OpenSSL client and our TlsConn server over in-memory transport.
    add_executable(http-tls-tests
        test/tls_test.cpp
        src/tls.cpp)
    target_include_directories(http-tls-tests
        PRIVATE ${HTTP_MODULE_DIR}/inc
        PRIVATE ${HTTP_MODULE_DIR}/src)
    target_link_libraries(http-tls-tests
        GTest::gtest_main GTest::gtest OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME http-tls-tests COMMAND http-tls-tests)

    # Worker-pool tests: pure C++17 threads, no ACE.
    add_executable(http-worker-tests
        test/worker_test.cpp
        src/worker.cpp)
    target_include_directories(http-worker-tests
        PRIVATE ${HTTP_MODULE_DIR}/inc
        PRIVATE ${HTTP_MODULE_DIR}/src)
    target_link_libraries(http-worker-tests
        GTest::gtest_main GTest::gtest pthread)
    add_test(NAME http-worker-tests COMMAND http-worker-tests)

    # D4 — handler tests: needs ds-server + datastore_client + ACE.
    # Only built when data-store is available.
    if(NOT HTTP_SKIP_DATA_STORE)
        add_executable(http-handler-tests
            test/handler_test.cpp
            src/handler.cpp
            src/router.cpp)
        target_link_libraries(http-handler-tests
            http_server_lib
            GTest::gtest_main GTest::gtest)
        add_test(NAME http-handler-tests COMMAND http-handler-tests)
    endif()
endif()

# ─────────────────────────── Install ───────────────────────────
include(GNUInstallDirs)
if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
    install(TARGETS iot-httpd
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES schemas/http.lua schemas/auth.lua
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)
endif()
