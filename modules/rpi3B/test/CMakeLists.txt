cmake_minimum_required(VERSION 3.16.3)
project(rpi3B_test CXX)

find_package(GTest REQUIRED)

# Test TUs use the C++20 abbreviated-template ctors (DRIVER(region)).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

# Only the test translation units here; the driver code comes from the
# rpi3b_driver library defined by the parent CMakeLists (no recompiling the
# src/*/*.cpp files, no double definitions).
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(rpi3B_test ${TEST_SOURCES})
target_link_libraries(rpi3B_test
                        rpi3b_driver
                        GTest::gtest
                        GTest::gtest_main
                        pthread)

include(GoogleTest)
if(RPI3B_TOP_LEVEL)
    # Native build: enumerate cases by running the binary on the host.
    gtest_discover_tests(rpi3B_test)
else()
    # Cross build (e.g. Yocto): never execute the target binary at build time.
    add_test(NAME rpi3B_test COMMAND rpi3B_test)
endif()

# Installed so an on-target systemd oneshot can run the suite as a self-test.
install(TARGETS rpi3B_test RUNTIME DESTINATION bin)
