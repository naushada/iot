cmake_minimum_required(VERSION 3.16.3)
project(containers CXX)

# Top-level (own build) vs. pulled in via add_subdirectory() from the iot tree.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(CONTAINERS_TOP_LEVEL ON)
else()
    set(CONTAINERS_TOP_LEVEL OFF)
endif()

option(CONTAINERS_BUILD_TESTS  "Build the containers gtest suite"            ${CONTAINERS_TOP_LEVEL})
option(CONTAINERS_BUILD_DAEMON "Build the iot-containerd runtime-shim daemon" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# nlohmann::json — vendored under apps/3rdparty/json/ (also a Yocto DEPENDS).
# Used by the pure registry manifest/index parser.
set(NLOHMANN_JSON_INCLUDE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../apps/3rdparty/json/single_include)

# Pure OCI image-reference + registry-v2 protocol core: no ACE, no network, no
# data-store (JSON via header-only nlohmann) — fully host-unit-testable. The
# HTTP transport (curl), overlay mount and crun lifecycle live in the
# iot-containerd daemon, gated by CONTAINERS_BUILD_DAEMON (forced ON in the iot
# image build). See apps/docs/tdd-device-containers.md.
add_library(containers_core STATIC
    src/image_ref.cpp
    src/registry.cpp
    src/oci_layer.cpp
    src/oci_bundle.cpp
    src/container_net.cpp)
target_include_directories(containers_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${NLOHMANN_JSON_INCLUDE})
target_compile_options(containers_core PRIVATE -Wall -Wextra)
add_library(containers::core ALIAS containers_core)

# ── Install the container.* schema (operator + daemon contract) ──
include(GNUInstallDirs)
if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
    install(FILES schemas/container.lua
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)
endif()

# ── iot-containerd daemon (privileged shim; reactor-driven, ACE-only) ──
# Built when CONTAINERS_BUILD_DAEMON=ON (forced on in the iot image build). Off
# by default for a plain `cmake modules/containers` core/test build.
if(CONTAINERS_BUILD_DAEMON)
    if(NOT DEFINED ACE_ROOT)
        set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
    endif()
    if(NOT TARGET datastore_client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../data-store
                         ${CMAKE_BINARY_DIR}/data-store)
    endif()

    # OpenSSL::Crypto — streaming SHA-256 of downloaded blobs (http_client).
    find_package(OpenSSL REQUIRED)

    add_executable(iot-containerd
        daemon/main.cpp
        daemon/containerd.cpp
        daemon/http_client.cpp
        daemon/registry_puller.cpp
        daemon/layer_store.cpp
        daemon/crun_runtime.cpp
        daemon/net_bridge.cpp)
    target_include_directories(iot-containerd PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/daemon
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${CMAKE_CURRENT_SOURCE_DIR}/../data-store/inc
        ${NLOHMANN_JSON_INCLUDE}
        ${ACE_ROOT}/include)
    target_link_directories(iot-containerd PRIVATE ${ACE_ROOT}/lib)
    # z: gzip layer decompression (zlib, already a base image dep, as in apps).
    target_link_libraries(iot-containerd PRIVATE
        containers_core datastore_client ACE pthread OpenSSL::Crypto z)

    if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
        install(TARGETS iot-containerd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()

if(CONTAINERS_BUILD_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(containers-tests
        test/image_ref_test.cpp
        test/registry_test.cpp
        test/oci_layer_test.cpp
        test/oci_bundle_test.cpp
        test/container_net_test.cpp)
    target_link_libraries(containers-tests containers_core GTest::gtest GTest::gtest_main pthread)
    include(GoogleTest)
    if(CONTAINERS_TOP_LEVEL)
        gtest_discover_tests(containers-tests)
    else()
        add_test(NAME containers-tests COMMAND containers-tests)
    endif()
endif()
