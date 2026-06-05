cmake_minimum_required(VERSION 3.16.3)
project(server_modules CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(openvpn)
add_subdirectory(lwm2m)
add_subdirectory(web)
