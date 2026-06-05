cmake_minimum_required(VERSION 3.16.3)
project(server_web CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(WEB_SERVER_DIR ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(
    ${WEB_SERVER_DIR}/inc
    ${WEB_SERVER_DIR}/../lwm2m/inc
)

add_library(server_web STATIC
    src/proxy.cpp)

target_include_directories(server_web PUBLIC inc)
target_link_libraries(server_web PUBLIC server_lwm2m pthread)
