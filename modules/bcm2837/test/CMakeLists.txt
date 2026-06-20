cmake_minimum_required(VERSION 3.16.3)
project(bcm2837_test CXX)

find_package(GTest REQUIRED)

# Test TUs use the C++20 region ctors.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

# Only the test translation units here; the driver code comes from the
# bcm2837_driver library defined by the parent CMakeLists (no recompiling the
# src/*/*.cpp files, no double definitions).
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(bcm2837_test ${TEST_SOURCES})
target_link_libraries(bcm2837_test
                        bcm2837_driver
                        GTest::gtest
                        GTest::gtest_main
                        pthread)

include(GoogleTest)
if(BCM2837_TOP_LEVEL)
    # Native build: enumerate cases by running the binary on the host.
    gtest_discover_tests(bcm2837_test)
else()
    # Cross build (e.g. Yocto): never execute the target binary at build time.
    add_test(NAME bcm2837_test COMMAND bcm2837_test)
endif()

# Installed so an on-target systemd oneshot can run the suite as a self-test.
install(TARGETS bcm2837_test RUNTIME DESTINATION bin)
