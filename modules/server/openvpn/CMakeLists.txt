cmake_minimum_required(VERSION 3.16.3)
project(server_openvpn CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OVPN_SERVER_DIR ${CMAKE_CURRENT_SOURCE_DIR})

if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

include_directories(${OVPN_SERVER_DIR}/inc ${ACE_ROOT}/include)

add_library(server_openvpn STATIC
    src/vpn_registry.cpp
    src/openvpn_server.cpp
    src/cert_authority.cpp)

target_include_directories(server_openvpn PUBLIC inc ${ACE_ROOT}/include)
target_link_directories(server_openvpn PUBLIC ${ACE_ROOT}/lib)
target_link_libraries(server_openvpn PUBLIC ACE pthread)

option(BUILD_SERVER_OPENVPN_TESTS "Build server/openvpn gtest suite" OFF)
if(BUILD_SERVER_OPENVPN_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    add_executable(vpn-registry-tests
        test/vpn_registry_test.cpp
        src/vpn_registry.cpp)
    target_include_directories(vpn-registry-tests
        PRIVATE ${OVPN_SERVER_DIR}/inc)
    target_link_libraries(vpn-registry-tests
        GTest::gtest_main GTest::gtest pthread)
    add_test(NAME vpn-registry-tests COMMAND vpn-registry-tests)

    # openvpn_server config rendering (pure; no ACE/process needed at link
    # since we only test build_server_config + cidr_to_net_mask). The .cpp
    # pulls ACE headers, so add the include + link ACE.
    add_executable(openvpn-server-tests
        test/openvpn_server_test.cpp
        src/openvpn_server.cpp)
    target_include_directories(openvpn-server-tests
        PRIVATE ${OVPN_SERVER_DIR}/inc ${ACE_ROOT}/include)
    target_link_directories(openvpn-server-tests PRIVATE ${ACE_ROOT}/lib)
    target_link_libraries(openvpn-server-tests
        GTest::gtest_main GTest::gtest ACE pthread)
    add_test(NAME openvpn-server-tests COMMAND openvpn-server-tests)

    # CertAuthority: sanitize_cn is pure; ensure()/mint_client() shell out to
    # openssl and SKIP at runtime if the CLI is absent.
    add_executable(cert-authority-tests
        test/cert_authority_test.cpp
        src/cert_authority.cpp)
    target_include_directories(cert-authority-tests
        PRIVATE ${OVPN_SERVER_DIR}/inc ${ACE_ROOT}/include)
    target_link_directories(cert-authority-tests PRIVATE ${ACE_ROOT}/lib)
    target_link_libraries(cert-authority-tests
        GTest::gtest_main GTest::gtest ACE pthread)
    add_test(NAME cert-authority-tests COMMAND cert-authority-tests)
endif()
