# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Autonomy®

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_library(unity STATIC ${PROJECT_SOURCE_DIR}/libs/unity/src/unity.c)
target_include_directories(unity PUBLIC ${PROJECT_SOURCE_DIR}/libs/unity/src)
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE)

file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
# ecat_run_argv is a stub on MSYS2/Cygwin
if(CMAKE_SYSTEM_NAME STREQUAL "MSYS" OR CMAKE_SYSTEM_NAME STREQUAL "CYGWIN")
    list(FILTER TEST_SOURCES EXCLUDE REGEX "test_proc\\.c$")
endif()
foreach(src ${TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    set(runner ${CMAKE_CURRENT_BINARY_DIR}/${name}_runner.c)
    add_custom_command(
        OUTPUT ${runner}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_runner.py ${src} ${runner}
        DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/gen_runner.py
    )
    add_executable(${name} ${src} ${runner})
    target_link_libraries(${name} PRIVATE etherdog_core unity)
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
