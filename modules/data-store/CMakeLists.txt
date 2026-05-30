cmake_minimum_required(VERSION 3.16.3)
project(data_store CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ACE_TAO install prefix — same as apps/CMakeLists.txt.
if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

set(DS_MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# nlohmann::json header-only, vendored under apps/3rdparty/json/.
# Used by ds-server only — the client lib stays JSON-free.
set(NLOHMANN_JSON_INCLUDE
    ${DS_MODULE_DIR}/../../apps/3rdparty/json/single_include)

include_directories(
    ${DS_MODULE_DIR}/inc
    ${ACE_ROOT}/include
    ${NLOHMANN_JSON_INCLUDE})

link_directories(${ACE_ROOT}/lib)

# --------------------------------------------------------------------
# libdatastore_client — ACE-backed I/O internally (LSOCK_Connector +
# LSOCK_Stream + Time_Value), but the public header (inc/data_store/
# client.hpp) stays free of ACE includes via a pimpl. Apps linking
# this static lib pull in ACE transitively at link time only.
# --------------------------------------------------------------------
add_library(datastore_client STATIC
    src/client/client.cpp
    src/proto/proto.cpp)

target_include_directories(datastore_client
    PUBLIC ${DS_MODULE_DIR}/inc)

target_link_libraries(datastore_client PUBLIC ACE pthread)

# --------------------------------------------------------------------
# ds-server — standalone daemon. Links ACE.
# --------------------------------------------------------------------
add_executable(ds-server
    src/server/main.cpp
    src/server/server.cpp
    src/server/session.cpp
    src/server/data_store.cpp
    src/server/worker.cpp
    src/server/worker_pool.cpp
    src/proto/proto.cpp)

target_link_libraries(ds-server
    ACE
    pthread)

# --------------------------------------------------------------------
# ds-cli — minimal debugging client. Links the client lib.
# --------------------------------------------------------------------
add_executable(ds-cli
    src/cli/ds_cli.cpp)

target_link_libraries(ds-cli
    datastore_client
    pthread)

# --------------------------------------------------------------------
# Tests — same gtest target as the iot suite. ACE + client lib both
# in scope because integration tests construct a server in-process.
# --------------------------------------------------------------------
if(BUILD_DATA_STORE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()

    add_executable(ds-tests
        test/data_store_test.cpp
        test/server_integration_test.cpp
        test/worker_pool_test.cpp
        test/protocol_test.cpp
        src/server/server.cpp
        src/server/session.cpp
        src/server/data_store.cpp
        src/server/worker.cpp
        src/server/worker_pool.cpp
        src/proto/proto.cpp
        src/client/client.cpp)

    target_link_libraries(ds-tests
        gtest_main
        gtest
        ACE
        pthread)

    add_test(NAME ds-tests COMMAND ds-tests)
endif()

install(TARGETS ds-server ds-cli
        RUNTIME DESTINATION /usr/local/bin)
install(TARGETS datastore_client
        ARCHIVE DESTINATION /usr/local/lib)
install(DIRECTORY inc/data_store
        DESTINATION /usr/local/include)
