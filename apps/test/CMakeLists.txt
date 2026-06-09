cmake_minimum_required(VERSION 3.16.3)
project(lwm2m_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -Wall -Wextra")

# Mirror apps/CMakeLists.txt so the sources under ../src compile with
# the same include paths + link deps. Diverging from main caused the
# pre-FUP-DS-8 breakage (missing nlohmann/json.hpp + ACE includes).
if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

find_package(GTest REQUIRED)
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

include_directories(
    ../inc
    test
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/json/single_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/tinydtls
    ${ACE_ROOT}/include
    ${LUA_INCLUDE_DIR}
)

link_directories(${ACE_ROOT}/lib)

enable_testing()

# Test sources + the minimum app-side sources they exercise. We
# deliberately leave out src/main.cpp (its own entry point) and the
# heavy LwM2M client/server wiring that needs UDP/DTLS/Mongo to come
# up — those land in their own integration tests when needed.
file(GLOB SOURCES "*.cpp"
                    "../src/coap_adapter.cpp"
                    "../src/cbor_adapter.cpp"
                    "../src/lwm2m_adapter.cpp"
                    "../src/lwm2m_codec_tlv.cpp"
                    "../src/lwm2m_codec_registry.cpp"
                    "../src/lwm2m_codec_linkformat.cpp"
                    "../src/lwm2m_codec_plaintext.cpp"
                    "../src/lwm2m_object_store.cpp"
                    "../src/lwm2m_registration.cpp"
                    "../src/lwm2m_registration_server.cpp"
                    "../src/lwm2m_registration_client.cpp"
                    "../src/lwm2m_bootstrap_server.cpp"
                    "../src/lwm2m_bootstrap_client.cpp"
                    "../src/lwm2m_dm_client.cpp"
                    "../src/lwm2m_dm_server.cpp"
                    "../src/lwm2m_observe.cpp"
                    "../src/lwm2m_codec_senml.cpp"
                    "../src/lwm2m_object_3_device.cpp"
                    "../src/lwm2m_object_stubs.cpp"
                    "../src/lwm2m_object_cert.cpp"
                    "../src/lwm2m_cert_chunk.cpp"
                    "../src/lua_config.cpp"
                    "../src/rpi_serial.cpp"
                    "../src/psk_gen.cpp"
                    "../src/provisioning_policy.cpp")

add_executable(lwm2m_test ${SOURCES})

target_link_libraries(lwm2m_test
    GTest::gtest_main
    GTest::gtest
    ACE
    pthread
    z
    ${LUA_LINK_LIBS})

add_test(NAME lwm2m_gtests COMMAND lwm2m_test)
