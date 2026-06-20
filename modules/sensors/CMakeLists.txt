cmake_minimum_required(VERSION 3.16.3)
project(sensors CXX)

# Top-level (own build) vs. pulled in via add_subdirectory() from the iot tree.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(SENSORS_TOP_LEVEL ON)
else()
    set(SENSORS_TOP_LEVEL OFF)
endif()

option(SENSORS_BUILD_TESTS  "Build the sensors gtest suite"         ${SENSORS_TOP_LEVEL})
option(SENSORS_BUILD_DAEMON "Build the iot-sensord producer daemon" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The drivers depend on the I2cTransport seam from modules/bcm2837. When this
# module is built on its own, pull bcm2837 in; in the iot build the apps
# CMakeLists has already added it (guard avoids a double add).
if(NOT TARGET bcm2837_driver)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../bcm2837
                     ${CMAKE_BINARY_DIR}/bcm2837)
endif()

# Per-chip drivers live in src/<chip>/<chip>.cpp.
file(GLOB SENSORS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*/*.cpp")

add_library(sensors_driver STATIC ${SENSORS_SOURCES})
target_include_directories(sensors_driver PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
target_link_libraries(sensors_driver PUBLIC bcm2837_driver)
target_compile_options(sensors_driver PRIVATE -Wall -Wextra)

add_library(sensors::driver ALIAS sensors_driver)

install(TARGETS sensors_driver ARCHIVE DESTINATION lib)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/inc/ DESTINATION include/sensors
        FILES_MATCHING PATTERN "*.hpp")

# ── iot-sensord producer daemon (privileged: maps I2C, publishes iot.sensor.*)
# Built in the integrated iot image (apps/CMakeLists sets SENSORS_BUILD_DAEMON
# and has already added datastore_client). Off by default for a plain
# `cmake modules/sensors` driver/test build.
if(SENSORS_BUILD_DAEMON)
    if(NOT DEFINED ACE_ROOT)
        set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
    endif()
    # data-store client (reuse the parent's target when present).
    if(NOT TARGET datastore_client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../data-store
                         ${CMAKE_BINARY_DIR}/data-store)
    endif()

    add_executable(iot-sensord
        daemon/main.cpp
        daemon/sensord.cpp)
    target_include_directories(iot-sensord PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/daemon
        ${CMAKE_CURRENT_SOURCE_DIR}/../data-store/inc
        ${ACE_ROOT}/include)
    target_link_directories(iot-sensord PRIVATE ${ACE_ROOT}/lib)
    target_link_libraries(iot-sensord PRIVATE
        sensors_driver datastore_client ACE pthread)

    include(GNUInstallDirs)
    if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
        install(TARGETS iot-sensord RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()

if(SENSORS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
