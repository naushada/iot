cmake_minimum_required(VERSION 3.16.3)
project(cloud_loadgen CXX)

# Standalone benchmark tool — NOT part of the cloud image build (built
# explicitly by run-bench.sh). Mirrors apps/CMakeLists.txt include/link so the
# ../src FSMs + transport compile identically to the shipped client.
# See apps/docs/tdd-cloud-load-benchmark.md.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -g -Wall -Wextra")

if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

find_package(Lua REQUIRED)
find_library(LUA_STATIC_LIB
             NAMES liblua5.3.a liblua.a
             PATHS /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu
                   /usr/lib /usr/local/lib
             NO_DEFAULT_PATH)
if(LUA_STATIC_LIB)
    set(LUA_LINK_LIBS ${LUA_STATIC_LIB} dl m)
else()
    set(LUA_LINK_LIBS ${LUA_LIBRARIES})
endif()

# tinydtls — vendored static lib (same as the main build).
if(NOT DEFINED TINYDTLS_LIBRARY)
    find_library(TINYDTLS_LIBRARY tinydtls
                 PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/tinydtls)
endif()
if(TINYDTLS_LIBRARY)
    set(TINYDTLS_LINK_LIB ${TINYDTLS_LIBRARY})
else()
    set(TINYDTLS_LINK_LIB ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/tinydtls/libtinydtls.a)
endif()

# Data-store client lib (dtls_adapter.cpp pulls in data_store/client.hpp).
# Build it from the sibling module, same as apps/CMakeLists.txt.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../modules/data-store
                 ${CMAKE_BINARY_DIR}/data-store)

include_directories(
    ../inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../../modules/data-store/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/json/single_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/tinydtls
    ${ACE_ROOT}/include
    ${LUA_INCLUDE_DIR}
)
link_directories(${ACE_ROOT}/lib
                 ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/tinydtls)

# The app-side sources the transport + FSMs pull in. Mirrors the known-good
# apps/test/CMakeLists.txt source list EXACTLY (so the link closes the same
# way), plus dtls_adapter.cpp which the gtest target doesn't need.
set(APP_SRC
    ../src/dtls_adapter.cpp
    ../src/coap_adapter.cpp
    ../src/cbor_adapter.cpp
    ../src/lwm2m_adapter.cpp
    ../src/lwm2m_codec_tlv.cpp
    ../src/lwm2m_codec_registry.cpp
    ../src/lwm2m_codec_linkformat.cpp
    ../src/lwm2m_codec_plaintext.cpp
    ../src/lwm2m_object_store.cpp
    ../src/lwm2m_registration.cpp
    ../src/lwm2m_registration_server.cpp
    ../src/lwm2m_registration_client.cpp
    ../src/lwm2m_bootstrap_server.cpp
    ../src/lwm2m_bootstrap_client.cpp
    ../src/lwm2m_dm_client.cpp
    ../src/lwm2m_dm_server.cpp
    ../src/lwm2m_observe.cpp
    ../src/lwm2m_codec_senml.cpp
    ../src/lwm2m_telemetry_pack.cpp
    ../src/lwm2m_send.cpp
    ../src/lwm2m_send_server.cpp
    ../src/lwm2m_send_uploader.cpp
    ../src/lwm2m_durable_sample_buffer.cpp
    ../src/lwm2m_object_3_device.cpp
    ../src/lwm2m_object_sensors.cpp
    ../src/lwm2m_object_stubs.cpp
    ../src/lwm2m_object_firmware.cpp
    ../src/lwm2m_object_cert.cpp
    ../src/lwm2m_cert_chunk.cpp
    ../src/lua_config.cpp
    ../src/rpi_serial.cpp
    ../src/psk_gen.cpp
    ../src/provisioning_policy.cpp
    ../src/tenant_policy.cpp
)

add_executable(cloud_loadgen cloud_loadgen.cpp ${APP_SRC})
target_link_libraries(cloud_loadgen
    datastore_client
    ${TINYDTLS_LINK_LIB}
    ACE
    pthread
    z
    ssl
    crypto
    sqlite3
    ${LUA_LINK_LIBS})
