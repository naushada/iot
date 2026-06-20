cmake_minimum_required(VERSION 3.16.3)
project(bcm2837 CXX)

# Detect whether this project is being built on its own or pulled in via
# add_subdirectory() (e.g. as the iot repo's vendored modules/bcm2837 tree).
# PROJECT_IS_TOP_LEVEL only exists from CMake 3.21, so compute it manually.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(BCM2837_TOP_LEVEL ON)
else()
    set(BCM2837_TOP_LEVEL OFF)
endif()

option(BCM2837_BUILD_APP   "Build the standalone bcm2837_demo executable" ${BCM2837_TOP_LEVEL})
option(BCM2837_BUILD_TESTS "Build the bcm2837 gtest self-test suite"      ${BCM2837_TOP_LEVEL})

# C++20 is required: the driver headers use explicit-template region ctors and
# the C++20-safe mmio_reg cell. The public headers stay C++17-consumable so the
# C++17 iot build can still link the library.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

# ── Driver library ──────────────────────────────────────────────────────────
# The actual peripheral drivers live in src/<peri>/<peri>.cpp.
file(GLOB BCM2837_DRIVER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*/*.cpp")

add_library(bcm2837_driver STATIC ${BCM2837_DRIVER_SOURCES})
target_include_directories(bcm2837_driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_options(bcm2837_driver PRIVATE -Wall -Wextra)

# Consumers (e.g. the iot apps build) link this alias.
add_library(bcm2837::driver ALIAS bcm2837_driver)

install(TARGETS bcm2837_driver ARCHIVE DESTINATION lib)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/inc/ DESTINATION include/bcm2837
        FILES_MATCHING PATTERN "*.hpp")

# ── Standalone executable (top-level builds only) ───────────────────────────
if(BCM2837_BUILD_APP)
    add_executable(bcm2837_demo src/main.cpp)
    target_link_libraries(bcm2837_demo PRIVATE bcm2837_driver)
endif()

# ── Test / self-test suite ──────────────────────────────────────────────────
if(BCM2837_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
