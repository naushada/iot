cmake_minimum_required(VERSION 3.16.3)
project(lwm2m)
find_package(GTest REQUIRED)


include_directories(../inc)
include_directories(test)

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
                    "../src/lwm2m_object_stubs.cpp")

enable_testing()

add_executable(lwm2m_test ${SOURCES})
target_link_libraries(lwm2m_test gtest_main)

target_link_libraries(lwm2m_test gtest)
target_link_libraries(lwm2m_test pthread)
target_link_libraries(lwm2m_test z)

add_test(lwm2m_gtests lwm2m_test)
gtest_discover_tests(lwm2m_test)
