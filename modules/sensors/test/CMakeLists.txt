cmake_minimum_required(VERSION 3.16.3)
project(sensors_test CXX)

find_package(GTest REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

# Test TUs only; the driver code comes from the sensors_driver library.
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(sensors_test ${TEST_SOURCES})
target_link_libraries(sensors_test
                        sensors_driver
                        GTest::gtest
                        GTest::gtest_main
                        pthread)

include(GoogleTest)
if(SENSORS_TOP_LEVEL)
    gtest_discover_tests(sensors_test)
else()
    add_test(NAME sensors_test COMMAND sensors_test)
endif()

install(TARGETS sensors_test RUNTIME DESTINATION bin)
