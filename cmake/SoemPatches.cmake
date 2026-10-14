# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Autonomy®
#
# Local patches to SOEM sources. Each patched file is built from a copy under the build
# directory, so the libs/soem submodule stays untouched. A patch that no longer applies
# stops the configure step.

find_package(Git REQUIRED)

# Patch SOEM's <source> (relative to SOEM_DIR) with <patch> and build the soem target from
# the patched copy. <must_not_contain> is a string the patched file must no longer contain.
function(etherdog_patch_soem_source source patch must_not_contain)
    set(work_dir ${CMAKE_BINARY_DIR}/soem-patch-work)
    set(out ${CMAKE_BINARY_DIR}/soem-patched/${source})

    file(REMOVE_RECURSE ${work_dir})
    get_filename_component(work_subdir ${work_dir}/${source} DIRECTORY)
    file(MAKE_DIRECTORY ${work_subdir})
    file(COPY_FILE ${SOEM_DIR}/${source} ${work_dir}/${source})

    # The ceiling keeps git from finding an enclosing repository, so it applies as plain patch.
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
            --unset=GIT_DIR --unset=GIT_WORK_TREE
            GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}
            ${GIT_EXECUTABLE} apply --verbose ${patch}
        WORKING_DIRECTORY ${work_dir}
        RESULT_VARIABLE apply_result
        OUTPUT_VARIABLE apply_output
        ERROR_VARIABLE apply_output
    )
    if(NOT apply_result EQUAL 0)
        message(FATAL_ERROR
            "EtherDOG - SOEM patch ${patch} does not apply to ${SOEM_DIR}/${source}. "
            "Update the patch for the pinned SOEM commit.\n${apply_output}")
    endif()

    file(READ ${work_dir}/${source} patched)
    string(FIND "${patched}" "${must_not_contain}" found_pos)
    if(NOT found_pos EQUAL -1)
        message(FATAL_ERROR "EtherDOG - patched ${source} still contains '${must_not_contain}'")
    endif()

    get_filename_component(out_dir ${out} DIRECTORY)
    file(MAKE_DIRECTORY ${out_dir})
    file(COPY_FILE ${work_dir}/${source} ${out} ONLY_IF_DIFFERENT)
    file(REMOVE_RECURSE ${work_dir})

    get_target_property(soem_sources soem SOURCES)
    list(FIND soem_sources ${source} index)
    if(index EQUAL -1)
        list(FIND soem_sources ${SOEM_DIR}/${source} index)
    endif()
    if(index EQUAL -1)
        message(FATAL_ERROR "EtherDOG - ${source} is not a source of the soem target")
    endif()
    list(REMOVE_AT soem_sources ${index})
    list(INSERT soem_sources ${index} ${out})
    set_property(TARGET soem PROPERTY SOURCES ${soem_sources})

    set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${patch} ${SOEM_DIR}/${source})
    message(STATUS "EtherDOG - SOEM ${source} patched with ${patch}")
endfunction()
