cmake_minimum_required(VERSION 3.16.3)
project(lwm2m C CXX)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -g -Wall -Wextra -O0")
set(CMAKE_VERBOSE_MAKEFILE ON)
#set(CMAKE_C_COMPILER /usr/bin/gcc)
#set(CMAKE_CXX_COMPILER /usr/bin/g++)

add_definitions(-DLWM2M_LITTLE_ENDIAN)

# MongoDB — compile-time guard for db_adapter.cpp and conditional linking.
# Set -DIOT_ENABLE_MONGO=OFF on the cmake command line (or via Yocto
# PACKAGECONFIG[mongo]) to drop mongocxx/bsoncxx from the build.
option(IOT_ENABLE_MONGO "Enable MongoDB registration mirror support" ON)
if(IOT_ENABLE_MONGO)
    add_definitions(-DIOT_ENABLE_MONGO)
endif()

# ACE_TAO install prefix — kept in sync with xpmile (see ../../xpmile/CMakeLists.txt).
# Override on the command line with -DACE_ROOT=/some/other/prefix if needed.
if(NOT DEFINED ACE_ROOT)
    set(ACE_ROOT /usr/local/ACE_TAO-7.0.0)
endif()

# Lua — required for apps/src/lua_config.cpp which now backs the
# apps/config/**/*.lua object-resource files (see apps/docs/cli.md).
# Prefer the static archive so the iot binary doesn't need
# liblua5.3.so.0 at runtime in stripped images (the iot:latest image
# isn't rebuilt yet to include liblua5.3-0). Falls back to whatever
# find_package(Lua) hands us if the .a isn't on disk.
find_package(Lua REQUIRED)
find_library(LUA_STATIC_LIB
             NAMES liblua5.3.a liblua.a
             PATHS /usr/lib/aarch64-linux-gnu
                   /usr/lib/x86_64-linux-gnu
                   /usr/lib
                   /usr/local/lib
             NO_DEFAULT_PATH)
if(LUA_STATIC_LIB)
    message(STATUS "Lua: static-linking ${LUA_STATIC_LIB}")
    set(LUA_LINK_LIBS ${LUA_STATIC_LIB} dl m)
else()
    set(LUA_LINK_LIBS ${LUA_LIBRARIES})
endif()

# Include External cmake file
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinydtls)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/single_include)
include_directories(/usr/local/include/mongocxx/v_noabi)
include_directories(/usr/local/include/bsoncxx/v_noabi)
include_directories(${ACE_ROOT}/include)
include_directories(${LUA_INCLUDE_DIR})

include_directories(inc)

# Data-store client lib — sibling module under ../modules/data-store.
# We add it as a subdirectory so the iot binary always links the
# in-tree libdatastore_client.a + its public headers; runtime use of
# the ds-server daemon is still optional per ds_config.cpp.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../modules/data-store
                 ${CMAKE_BINARY_DIR}/data-store)

# OpenVPN client daemon (L12). Sibling under ../modules/openvpn/client.
# Adding it here means a top-level `cmake apps && make install` lands
# the openvpn-client binary + its vpn.lua schema alongside everything
# else. Standalone module build at modules/openvpn/client/ still works.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../modules/openvpn/client
                 ${CMAKE_BINARY_DIR}/openvpn-client)

# net-router daemon (L13). Sibling under ../modules/net/router.
# Brings the net-router binary + net.lua schema into the same install
# tree as everything else.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../modules/net/router
                 ${CMAKE_BINARY_DIR}/net-router)

# wifi-client daemon (L15). Sibling under ../modules/wan/wifi/client.
# Owns the wpa_supplicant(8) + DHCP client lifecycle for one wifi
# iface; publishes wifi.* state into ds-server. Standalone module
# build at modules/wan/wifi/client/ still works.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../modules/wan/wifi/client
                 ${CMAKE_BINARY_DIR}/wifi-client)

# http-server daemon (L18). Sibling under ../modules/http-server.
# Lands the iot-httpd binary + http.lua schema in the same install
# tree. data-store is already added above, so http-server reuses the
# existing datastore_client target (its add_subdirectory is guarded by
# `NOT TARGET datastore_client`). Standalone module build still works.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../modules/http-server
                 ${CMAKE_BINARY_DIR}/http-server)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../modules/data-store/inc)

link_directories(${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinydtls)
link_directories(${ACE_ROOT}/lib)

# tinydtls — prefer sysroot when cross-compiling (Yocto sets
# -DTINYDTLS_LIBRARY=...), fall back to vendored submodule.
if(NOT DEFINED TINYDTLS_LIBRARY)
    find_library(TINYDTLS_LIBRARY tinydtls
                 PATHS ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinydtls
                 NO_DEFAULT_PATH)
    if(NOT TINYDTLS_LIBRARY)
        find_library(TINYDTLS_LIBRARY tinydtls)
    endif()
endif()
if(TINYDTLS_LIBRARY)
    set(TINYDTLS_LINK_LIB ${TINYDTLS_LIBRARY})
else()
    set(TINYDTLS_LINK_LIB ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tinydtls/libtinydtls.a)
endif()

#all files from src directory (recursive — picks up src/cli/**/*.cpp too).
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(lwm2m ${SOURCES})
target_link_libraries(lwm2m
                        ${TINYDTLS_LINK_LIB}
                        datastore_client
                        ACE
                        z
                        readline
                        pthread
                        bsoncxx
                        mongocxx
                        ssl
                        crypto
                        ${LUA_LINK_LIBS})

option(BUILD_APPS_TESTS "Build apps-level gtest suite (lwm2m_test)" OFF)
if(BUILD_APPS_TESTS)
    add_subdirectory(test)
endif()

include(GNUInstallDirs)

# Drop the CONFIGURATIONS Release filter so `make install` works in
# Debug builds too — packagers don't always rebuild as Release.
install(TARGETS lwm2m
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ──────────────────────────── packaging assets ─────────────────────────
# IOT_PACKAGING_INSTALL toggles the systemd + /etc/iot/* install. Default
# ON so a `make install` produces a deployable tree; flip to OFF for
# dev installs that should not touch /lib/systemd or /etc/iot.
option(IOT_PACKAGING_INSTALL "Install systemd units, env files, config examples" ON)

if(IOT_PACKAGING_INSTALL)
    # systemd unit files. /lib/systemd/system is the conventional
    # vendor path; distros' own units live at /etc/systemd/system
    # and override us, which is fine.
    install(FILES
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/systemd/iot-ds.service
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/systemd/iot-lwm2m-client.service
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/systemd/iot-lwm2m-server.service
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/systemd/iot-openvpn-client.service
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/systemd/iot-net-router.service
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/systemd/iot-wifi-client.service
            DESTINATION lib/systemd/system)

    # tmpfiles.d fallback for hosts where RuntimeDirectory= doesn't
    # fire (older systemd, sysvinit shims). MUST land under plain
    # `lib/tmpfiles.d`, NOT `${CMAKE_INSTALL_LIBDIR}/tmpfiles.d` —
    # multi-arch hosts make LIBDIR=lib/<triplet> which systemd-tmpfiles
    # doesn't scan.
    install(FILES
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/systemd/iot.conf
            DESTINATION lib/tmpfiles.d)

    # EnvironmentFile for each lwm2m unit. Use FULL_SYSCONFDIR so the
    # files land at /etc/iot/ when --prefix=/usr (cmake's GNUInstallDirs
    # special-cases /usr to make sysconfdir an absolute /etc). The
    # relative CMAKE_INSTALL_SYSCONFDIR would have placed these at
    # /usr/etc/iot/ which no service reads.
    install(FILES
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/etc-iot/lwm2m-client.env
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/etc-iot/lwm2m-server.env
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/etc-iot/openvpn-client.env
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/etc-iot/net-router.env
                ${CMAKE_CURRENT_SOURCE_DIR}/../packaging/etc-iot/wifi-client.env
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot)

    # Object-resource config templates. Land each .lua as .lua.example
    # so an operator copy + edit is required — packaging upgrades won't
    # silently clobber edited config. Renaming has to be per-file
    # because cmake's install(FILES RENAME) only takes one source.
    set(IOT_CONFIG_DIR ${CMAKE_INSTALL_FULL_SYSCONFDIR}/iot/config)
    foreach(subdir securityObject serverObject deviceObject accessControlObject firmwareObject)
        file(GLOB _lua_files
             RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/config/${subdir}
             ${CMAKE_CURRENT_SOURCE_DIR}/config/${subdir}/*.lua)
        foreach(_f ${_lua_files})
            install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/config/${subdir}/${_f}
                    DESTINATION ${IOT_CONFIG_DIR}/${subdir}
                    RENAME ${_f}.example)
        endforeach()
    endforeach()
endif()