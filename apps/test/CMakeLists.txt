cmake_minimum_required(VERSION 3.16.3)
project(lwm2m)
find_package(GTest REQUIRED)
find_package(Lua  REQUIRED)


include_directories(../inc)
include_directories(test)
include_directories(${LUA_INCLUDE_DIR})

#Adding Complier flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -Wall -Wextra")

#However, the file(GLOB...) allows for wildcard additions:
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
                    "../src/lua_config.cpp")

enable_testing()

add_executable(lwm2m_test ${SOURCES})
target_link_libraries(lwm2m_test gtest_main)

target_link_libraries(lwm2m_test gtest)
target_link_libraries(lwm2m_test pthread)
target_link_libraries(lwm2m_test z)
find_library(LUA_STATIC_LIB
             NAMES liblua5.3.a liblua.a
             PATHS /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu
                   /usr/lib /usr/local/lib NO_DEFAULT_PATH)
if(LUA_STATIC_LIB)
    target_link_libraries(lwm2m_test ${LUA_STATIC_LIB} dl m)
else()
    target_link_libraries(lwm2m_test ${LUA_LIBRARIES})
endif()

add_test(lwm2m_gtests lwm2m_test)
gtest_discover_tests(lwm2m_test)
