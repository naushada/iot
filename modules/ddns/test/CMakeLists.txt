cmake_minimum_required(VERSION 3.16.3)
project(ddns_test CXX)

find_package(GTest REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

# Test TUs only; the logic under test comes from ddns_core. ddns_core links
# ACE (ACE_Process in http_client) + OpenSSL::Crypto (propagated PUBLIC).
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(ddns_test ${TEST_SOURCES})
target_link_directories(ddns_test PRIVATE ${ACE_ROOT}/lib)
target_link_libraries(ddns_test
                        ddns_core
                        ACE
                        GTest::gtest
                        GTest::gtest_main
                        pthread)

include(GoogleTest)
if(DDNS_TOP_LEVEL)
    gtest_discover_tests(ddns_test)
else()
    add_test(NAME ddns_test COMMAND ddns_test)
endif()

install(TARGETS ddns_test RUNTIME DESTINATION bin)
