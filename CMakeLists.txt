# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Autonomy®
#
# EtherDOG: standalone EtherCAT master service built on SOEM.

cmake_minimum_required(VERSION 3.13)
project(etherdog C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(ETHERDOG_BUILD_TESTS "Build the unit tests" OFF)

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION ETHERDOG_VERSION LIMIT_COUNT 1)

# =============================================================================
# SOEM (git submodule libs/soem)
# =============================================================================

set(SOEM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs/soem)
set(SOEM_BUILD_SAMPLES OFF CACHE BOOL "Disable SOEM samples" FORCE)

# SOEM has no MSYS2/Cygwin platform file; generate one from the Win32 OSAL/OSHW (w32api).
if(CMAKE_SYSTEM_NAME STREQUAL "MSYS" OR CMAKE_SYSTEM_NAME STREQUAL "CYGWIN")
    set(_SOEM_PLATFORM_CMAKE "${SOEM_DIR}/cmake/${CMAKE_SYSTEM_NAME}.cmake")
    message(STATUS "EtherDOG - Generating ${CMAKE_SYSTEM_NAME}.cmake for SOEM (Win32 OSAL/OSHW via w32api)")
    file(WRITE "${_SOEM_PLATFORM_CMAKE}" "\
# Auto-generated for ${CMAKE_SYSTEM_NAME} -- based on Windows.cmake\n\
# Uses Win32 OSAL/OSHW accessible through w32api on MSYS2/Cygwin.\n\
\n\
target_sources(soem PRIVATE\n\
  osal/win32/osal.c\n\
  osal/win32/osal_defs.h\n\
  oshw/win32/oshw.c\n\
  oshw/win32/oshw.h\n\
  oshw/win32/nicdrv.c\n\
  oshw/win32/nicdrv.h\n\
)\n\
\n\
target_include_directories(soem PUBLIC\n\
  $<BUILD_INTERFACE:\${SOEM_SOURCE_DIR}/osal/win32>\n\
  $<BUILD_INTERFACE:\${SOEM_SOURCE_DIR}/oshw/win32>\n\
  $<BUILD_INTERFACE:\${SOEM_SOURCE_DIR}/oshw/win32/wpcap/Include>\n\
  $<INSTALL_INTERFACE:include/soem>\n\
)\n\
\n\
target_compile_options(soem PRIVATE\n\
  $<$<C_COMPILER_ID:GNU>:-std=c11>\n\
)\n\
\n\
# WIN32: not predefined by MSYS2 GCC; wpcap needs it. HAVE_U_INT*_T: MSYS2 already has them.\n\
# __USE_W32_SOCKETS: take select/gethostname from winsock2.h, not the POSIX headers.\n\
target_compile_definitions(soem PUBLIC\n\
  WIN32\n\
  __USE_W32_SOCKETS\n\
  HAVE_U_INT8_T\n\
  HAVE_U_INT16_T\n\
  HAVE_U_INT32_T\n\
  HAVE_U_INT64_T\n\
  # pcap-stdinc.h maps snprintf to _snprintf without _MSC_VER; map it back.\n\
  _snprintf=snprintf\n\
  _vsnprintf=vsnprintf\n\
)\n\
\n\
foreach(target IN ITEMS\n\
    soem\n\
    ec_sample\n\
    eepromtool\n\
    eni_test\n\
    eoe_test\n\
    firm_update\n\
    simple_ng\n\
    slaveinfo)\n\
  if (TARGET \${target})\n\
    target_compile_options(\${target} PRIVATE\n\
      $<$<C_COMPILER_ID:GNU>:\n\
      -Wall\n\
      -Wextra\n\
      -Wno-unused-parameter\n\
      >\n\
    )\n\
  endif()\n\
endforeach()\n\
\n\
if(CMAKE_SIZEOF_VOID_P EQUAL 8)\n\
  set(WPCAP_LIB_PATH \${SOEM_SOURCE_DIR}/oshw/win32/wpcap/Lib/x64)\n\
  target_link_libraries(soem PUBLIC\n\
    \${WPCAP_LIB_PATH}/wpcap.lib\n\
    \${WPCAP_LIB_PATH}/Packet.lib\n\
  )\n\
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)\n\
  set(WPCAP_LIB_PATH \${SOEM_SOURCE_DIR}/oshw/win32/wpcap/Lib)\n\
  target_link_libraries(soem PUBLIC\n\
    \${WPCAP_LIB_PATH}/libwpcap.a\n\
    \${WPCAP_LIB_PATH}/libpacket.a\n\
  )\n\
endif()\n\
\n\
target_link_libraries(soem PUBLIC ws2_32 winmm)\n\
\n\
install(FILES\n\
  osal/win32/osal_defs.h\n\
  oshw/win32/nicdrv.h\n\
  DESTINATION include/soem\n\
)\n\
")
    unset(_SOEM_PLATFORM_CMAKE)
endif()

add_subdirectory(${SOEM_DIR} ${CMAKE_CURRENT_BINARY_DIR}/soem)

# =============================================================================
# EtherDOG core library (everything but main, so tests can link it)
# =============================================================================

add_library(etherdog_core STATIC
    src/bus.c
    src/config.c
    src/control.c
    src/data.c
    src/dgram.c
    src/iface_state.c
    src/layout.c
    src/log.c
    src/master.c
    src/proc.c
    src/cjson/cJSON.c
)

target_include_directories(etherdog_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(etherdog_core PUBLIC EDOG_VERSION="${ETHERDOG_VERSION}")
target_compile_options(etherdog_core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

find_package(Threads REQUIRED)
# SOEM links ws2_32, which also exports socket/bind/sendto; list cygwin first so the POSIX ones win
if(CYGWIN OR CMAKE_SYSTEM_NAME STREQUAL "MSYS")
    target_link_libraries(etherdog_core PUBLIC cygwin)
endif()
target_link_libraries(etherdog_core PUBLIC soem Threads::Threads)
if(UNIX AND NOT APPLE AND NOT CYGWIN AND NOT CMAKE_SYSTEM_NAME STREQUAL "MSYS")
    target_link_libraries(etherdog_core PUBLIC rt)
endif()

add_executable(etherdog src/main.c)
target_link_libraries(etherdog PRIVATE etherdog_core)
target_compile_options(etherdog PRIVATE -Wall -Wextra -Wno-unused-parameter)

install(TARGETS etherdog RUNTIME DESTINATION bin)
install(FILES LICENSE README.md DESTINATION share/doc/etherdog)
install(FILES docs/PROTOCOL.md docs/BUSCONFIG.md DESTINATION share/doc/etherdog)

if(ETHERDOG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
