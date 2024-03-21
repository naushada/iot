cmake_minimum_required(VERSION 3.16.3)
project(lwm2m C CXX)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -g -Wall -Wextra -O0")
set(CMAKE_VERBOSE_MAKEFILE ON)
#set(CMAKE_C_COMPILER /usr/bin/gcc)
#set(CMAKE_CXX_COMPILER /usr/bin/g++)

add_definitions(-DLWM2M_LITTLE_ENDIAN)

# Include External cmake file
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinydtls)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/single_include)
include_directories(/usr/local/include/mongocxx/v_noabi)
include_directories(/usr/local/include/bsoncxx/v_noabi)

include_directories(inc)

link_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinydtls)

#all files from src directory, the file(GLOB...) allows for wildcard additions:
file(GLOB SOURCES "src/*.cpp")

add_executable(lwm2m ${SOURCES})
target_link_libraries(lwm2m 
                        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinydtls/libtinydtls.a 
                        z
                        readline
                        pthread
                        bsoncxx
                        mongocxx
                        ssl
                        crypto)

add_subdirectory(test)

install(TARGETS lwm2m
        CONFIGURATIONS Release
        RUNTIME DESTINATION /usr/local/bin
)