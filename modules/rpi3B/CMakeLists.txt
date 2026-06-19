cmake_minimum_required(VERSION 3.16.3)
project(rpi3B CXX)

# Detect whether this project is being built on its own or pulled in via
# add_subdirectory() (e.g. as a submodule of the iot repo). PROJECT_IS_TOP_LEVEL
# only exists from CMake 3.21, so compute it manually for the 3.16 floor.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(RPI3B_TOP_LEVEL ON)
else()
    set(RPI3B_TOP_LEVEL OFF)
endif()

option(RPI3B_BUILD_APP   "Build the standalone rpi3Bdriver executable" ${RPI3B_TOP_LEVEL})
option(RPI3B_BUILD_TESTS "Build the rpi3B gtest self-test suite"       ${RPI3B_TOP_LEVEL})

# C++20 is required: the driver headers use abbreviated-template constructors
# (e.g. `GPIO(auto region)`) for test/region placement. The old -std=c++17 root
# could never compile the drivers for this reason (see DRIVER_REVIEW.md §2.1).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

# ── Driver library ──────────────────────────────────────────────────────────
# The actual peripheral drivers live in src/<peri>/<peri>.cpp. The previous
# glob ("src/*.cpp") missed every one of them and only compiled main.cpp
# (see docs/DRIVER_REVIEW.md §2.1); src/*/*.cpp picks them all up.
file(GLOB RPI3B_DRIVER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*/*.cpp")

add_library(rpi3b_driver STATIC ${RPI3B_DRIVER_SOURCES})
target_include_directories(rpi3b_driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_compile_options(rpi3b_driver PRIVATE -Wall -Wextra)

# Consumers (e.g. the iot apps build) link this alias.
add_library(rpi3b::driver ALIAS rpi3b_driver)

install(TARGETS rpi3b_driver ARCHIVE DESTINATION lib)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/inc/ DESTINATION include/rpi3b
        FILES_MATCHING PATTERN "*.hpp")

# ── Standalone executable (top-level builds only) ───────────────────────────
if(RPI3B_BUILD_APP)
    add_executable(rpi3Bdriver src/main.cpp)
    target_link_libraries(rpi3Bdriver PRIVATE rpi3b_driver)
endif()

# ── Test / self-test suite ──────────────────────────────────────────────────
if(RPI3B_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
