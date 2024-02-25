cmake_minimum_required(VERSION 3.16.3)
project(ammserver C CXX)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -g -Wall -Wextra -O0")
set(CMAKE_VERBOSE_MAKEFILE ON)
#set(CMAKE_C_COMPILER /usr/bin/gcc)
#set(CMAKE_CXX_COMPILER /usr/bin/g++)

add_definitions(-DLWM2M_LITTLE_ENDIAN)

# Include External cmake file
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinydtls)
#include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/legato-3rdParty-tinycbor/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/single_include)

include_directories(inc)

link_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinydtls)
#link_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/legato-3rdParty-tinycbor/lib)

#all files from src directory, the file(GLOB...) allows for wildcard additions:
file(GLOB SOURCES "src/*.cpp")

add_executable(ammserver ${SOURCES})
#${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/legato-3rdParty-tinycbor/lib/libtinycbor.a
target_link_libraries(ammserver 
                        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinydtls/libtinydtls.a 
                        z)

add_subdirectory(test)

install(TARGETS ammserver
        CONFIGURATIONS Release
        RUNTIME DESTINATION /usr/local/bin
)