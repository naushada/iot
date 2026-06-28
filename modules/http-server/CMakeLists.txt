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
    ${HTTP_MODULE_DIR}/../server/lwm2m/inc
    ${HTTP_MODULE_DIR}/../server/openvpn/inc
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
    src/handler_cloud.cpp
    src/tenant_scope.cpp
    src/handler_proxy.cpp
    src/handler_shell.cpp
    src/shell.cpp
    src/auth.cpp
    src/tls.cpp
    src/worker.cpp)

target_include_directories(http_server_lib PUBLIC src)
target_link_libraries(http_server_lib
    PUBLIC datastore_client ACE pthread OpenSSL::SSL OpenSSL::Crypto)

# forkpty(3) for the device-ui Terminal lives in libutil on glibc/Linux
# (it's in libSystem on macOS/BSD, so only link it on Linux).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(http_server_lib PUBLIC util)
endif()

# ─────────────────────────── Binary ────────────────────────────
add_executable(iot-httpd src/main.cpp)
target_link_libraries(iot-httpd PRIVATE http_server_lib)

# Bake the release version (the repo-root VERSION file, single source of truth,
# bumped per release alongside the git tag). iot-httpd writes it to ds
# (iot.version) at startup so the UI can show what's actually running — after an
# OTA the restarted new binary self-reports the freshly-installed version.
# Append a short git sha when building from a checkout; fall back gracefully when
# neither the file nor .git is present (e.g. partial build contexts).
set(_iot_version_file "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
if(EXISTS "${_iot_version_file}")
    file(STRINGS "${_iot_version_file}" IOT_VERSION LIMIT_COUNT 1)
else()
    set(IOT_VERSION "0.0.0-dev")
endif()
find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/../.." rev-parse --short HEAD
        OUTPUT_VARIABLE _iot_git_sha OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_iot_git_sha)
        set(IOT_VERSION "${IOT_VERSION}+${_iot_git_sha}")
    endif()
endif()
message(STATUS "iot-httpd version: ${IOT_VERSION}")
target_compile_definitions(iot-httpd PRIVATE IOT_VERSION="${IOT_VERSION}")

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

        # Cloud API tests — exercise install_cloud_handlers (tenant read
        # isolation). Needs the server-side registry/bootstrap/vpn sources the
        # handler drives (http_server_lib drops handler_cloud.o for iot-httpd as
        # unreferenced, so those symbols aren't otherwise linked).
        add_executable(http-cloud-api-tests
            test/cloud_api_test.cpp
            ${HTTP_MODULE_DIR}/../server/lwm2m/src/endpoint_registry.cpp
            ${HTTP_MODULE_DIR}/../server/lwm2m/src/bootstrap.cpp
            ${HTTP_MODULE_DIR}/../server/openvpn/src/vpn_registry.cpp)
        target_link_libraries(http-cloud-api-tests
            http_server_lib
            GTest::gtest_main GTest::gtest)
        add_test(NAME http-cloud-api-tests COMMAND http-cloud-api-tests)
    endif()
endif()

# ─────────────────────────── Install ───────────────────────────
include(GNUInstallDirs)
if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
    install(TARGETS iot-httpd
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES schemas/http.lua schemas/auth.lua
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)

    # ── SPA static files (iot-ui Angular build output) ─────────
    set(IOT_WWW_DIR "${CMAKE_INSTALL_DATAROOTDIR}/iot/www"
        CACHE PATH "Install path for the SPA static files")
    set(UI_DIST "${HTTP_MODULE_DIR}/../../iot-ui/dist/iot-ui")
    if(EXISTS "${UI_DIST}/index.html")
        install(DIRECTORY "${UI_DIST}/"
                DESTINATION "${IOT_WWW_DIR}"
                FILES_MATCHING PATTERN "*")
        message(STATUS "iot-ui SPA found — installing to ${IOT_WWW_DIR}")
    else()
        message(STATUS "iot-ui SPA not built at ${UI_DIST} — skipping install")
    endif()
endif()
