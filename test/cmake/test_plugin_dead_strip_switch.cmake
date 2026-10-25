#[[
Contract test for the plug-in dead-strip switch.

Configures a throwaway project that includes PulpPluginFormats.cmake and
applies its module link hygiene to two stand-in module targets, then reads
each target's evaluated LINK_OPTIONS. Three configurations must disagree in
exactly the expected way:

  - default Release: both modules link with -dead_strip (the control; a check
    that never sees the flag would pass every opt-out case for free);
  - Release with one module's PULP_DEAD_STRIP property set OFF: only that
    module loses it;
  - Release with -DPULP_DEAD_STRIP_PLUGINS=OFF: neither carries it.

macOS only: the flag is Apple-specific and the hygiene applies nothing
elsewhere.
]]

if(NOT CMAKE_HOST_APPLE)
    message(STATUS "dead-strip switch: not an Apple host; skipping")
    return()
endif()

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT PULP_DEAD_STRIP_TEST_DIR)
    set(PULP_DEAD_STRIP_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/plugin-dead-strip-switch")
endif()
set(_src "${PULP_DEAD_STRIP_TEST_DIR}/src")
file(REMOVE_RECURSE "${PULP_DEAD_STRIP_TEST_DIR}")
file(MAKE_DIRECTORY "${_src}")
file(WRITE "${_src}/module.c" "int pulp_dead_strip_fixture(void) { return 1; }\n")
file(WRITE "${_src}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.24)
project(pulp_dead_strip_switch C)
include(\"${_repo_root}/tools/cmake/PulpPluginFormats.cmake\")
foreach(_m Keep Drop)
    add_library(\${_m} MODULE module.c)
    _pulp_plugin_module_link_hygiene(\${_m})
endforeach()
if(DROP_ONE)
    set_target_properties(Drop PROPERTIES PULP_DEAD_STRIP OFF)
endif()
foreach(_m Keep Drop)
    file(GENERATE OUTPUT \"\${CMAKE_BINARY_DIR}/\${_m}.options\"
        CONTENT \"$<TARGET_PROPERTY:\${_m},LINK_OPTIONS>\")
endforeach()
")

function(_configure label expect_keep expect_drop)
    set(_bin "${PULP_DEAD_STRIP_TEST_DIR}/${label}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_src}" -B "${_bin}" -DCMAKE_BUILD_TYPE=Release ${ARGN}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${label}: configure failed\n${_out}\n${_err}")
    endif()
    foreach(_m Keep Drop)
        file(READ "${_bin}/${_m}.options" _opts)
        string(REPLACE "-dead_strip_dylibs" "" _opts "${_opts}")
        string(FIND "${_opts}" "-dead_strip" _at)
        if(_m STREQUAL "Keep")
            set(_want ${expect_keep})
        else()
            set(_want ${expect_drop})
        endif()
        if(_want AND _at EQUAL -1)
            message(FATAL_ERROR "${label}: ${_m} should link with -dead_strip; LINK_OPTIONS: ${_opts}")
        elseif(NOT _want AND NOT _at EQUAL -1)
            message(FATAL_ERROR "${label}: ${_m} should not link with -dead_strip; LINK_OPTIONS: ${_opts}")
        endif()
    endforeach()
    message(STATUS "dead-strip switch: ${label} ok")
endfunction()

_configure(default TRUE TRUE)
_configure(property-off TRUE FALSE -DDROP_ONE=ON)
_configure(option-off FALSE FALSE -DPULP_DEAD_STRIP_PLUGINS=OFF)
