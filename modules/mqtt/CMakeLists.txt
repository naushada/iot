cmake_minimum_required(VERSION 3.16.3)
project(iotmqtt CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OFF by default: only the device (Yocto) build enables this (it has
# libmosquitto from meta-networking). The cloud Docker builder has no
# libmosquitto, so apps/CMakeLists must NOT force this ON — the recipe passes
# -DMQTT_BUILD_DAEMON=ON. A plain add_subdirectory just installs the schema.
option(MQTT_BUILD_DAEMON "Build the iot-mqttd MQTT mirror daemon" OFF)

include(GNUInstallDirs)
if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
    install(FILES schemas/mqtt.lua
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/ds-schemas)
endif()

if(MQTT_BUILD_DAEMON)
    if(NOT DEFINED ACE_ROOT)
        set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
    endif()
    if(NOT TARGET datastore_client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../data-store
                         ${CMAKE_BINARY_DIR}/data-store)
    endif()

    add_executable(iot-mqttd
        daemon/main.cpp
        daemon/mqtt_mirror.cpp)
    target_include_directories(iot-mqttd PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/daemon
        ${CMAKE_CURRENT_SOURCE_DIR}/../data-store/inc
        ${ACE_ROOT}/include)
    target_link_directories(iot-mqttd PRIVATE ${ACE_ROOT}/lib)
    # libmosquitto (meta-networking) provides -lmosquitto.
    target_link_libraries(iot-mqttd PRIVATE
        datastore_client ACE mosquitto pthread)

    if(NOT DEFINED IOT_PACKAGING_INSTALL OR IOT_PACKAGING_INSTALL)
        install(TARGETS iot-mqttd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()
