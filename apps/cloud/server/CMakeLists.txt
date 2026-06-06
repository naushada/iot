cmake_minimum_required(VERSION 3.16.3)
project(cloud_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

set(CLOUD_SERVER_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# nlohmann::json — vendored
set(NLOHMANN_JSON_INCLUDE
    ${CLOUD_SERVER_DIR}/../../3rdparty/json/single_include)

include_directories(
    ${CLOUD_SERVER_DIR}/../../../modules/data-store/inc
    ${CLOUD_SERVER_DIR}/../../../modules/server/lwm2m/inc
    ${CLOUD_SERVER_DIR}/../../../modules/server/openvpn/inc
    ${NLOHMANN_JSON_INCLUDE}
    ${ACE_ROOT}/include
)

add_executable(iot-cloudd
    src/main.cpp
    ../../../modules/server/lwm2m/src/endpoint_registry.cpp
    ../../../modules/server/lwm2m/src/bootstrap.cpp
    ../../../modules/server/openvpn/src/vpn_registry.cpp)

if(DEFINED DATASTORE_LIB)
    set(DS_LINK_LIB ${DATASTORE_LIB})
else()
    set(DS_LINK_LIB datastore_client)
endif()

target_link_libraries(iot-cloudd
    ${DS_LINK_LIB} ACE pthread)

# ── Install ──────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS iot-cloudd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
