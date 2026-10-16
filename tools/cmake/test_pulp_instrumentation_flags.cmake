# Coverage compile flags per compiler driver, for PulpInstrumentation.cmake.
#
# Script mode (`cmake -P`). The module is include-guarded and calls
# project-only commands, so each driver runs in a child `cmake -P` of this
# file with those commands stubbed to record their arguments.

cmake_minimum_required(VERSION 3.20)

if(DEFINED PULP_INSTRUMENTATION_PROBE_VARIANT)
    # Child: configure the module as the named driver and print its flags.
    function(add_compile_options)
        message(STATUS "compile-options:${ARGN}")
    endfunction()
    function(add_link_options)
    endfunction()
    set(PULP_ENABLE_COVERAGE ON)
    set(CMAKE_BUILD_TYPE Debug)
    set(CMAKE_CXX_COMPILER_ID Clang)
    set(CMAKE_C_COMPILER_ID Clang)
    set(CMAKE_CXX_COMPILER_FRONTEND_VARIANT ${PULP_INSTRUMENTATION_PROBE_VARIANT})
    include(${CMAKE_CURRENT_LIST_DIR}/PulpInstrumentation.cmake)
    return()
endif()

set(_failures 0)

function(flags_for variant out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DPULP_INSTRUMENTATION_PROBE_VARIANT=${variant}
                -P ${CMAKE_CURRENT_LIST_FILE}
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
    string(REGEX MATCH "compile-options:[^\n]*" _line "${_out}${_err}")
    if(NOT _rc EQUAL 0 OR NOT _line)
        message(FATAL_ERROR "probe for ${variant} failed (${_rc}):\n${_out}${_err}")
    endif()
    set(${out} "${_line}" PARENT_SCOPE)
endfunction()

function(expect what holds)
    if(holds)
        message(STATUS "  ok   ${what}")
    else()
        message(STATUS "  FAIL ${what}")
        math(EXPR _f "${_failures} + 1")
        set(_failures "${_f}" PARENT_SCOPE)
    endif()
endfunction()

# TRUE when ${flags} carries ${flag} as a whole option.
function(has_flag out flags flag)
    string(REGEX REPLACE "^compile-options:" "" _list "${flags}")
    if("${flag}" IN_LIST _list)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

flags_for(GNU _gnu)
flags_for(MSVC _msvc)
message(STATUS "GNU driver:  ${_gnu}")
message(STATUS "MSVC driver: ${_msvc}")

has_flag(_gnu_g1 "${_gnu}" -g1)
has_flag(_gnu_o0 "${_gnu}" -O0)
has_flag(_msvc_g1 "${_msvc}" -g1)
has_flag(_msvc_o0 "${_msvc}" -O0)
has_flag(_msvc_map "${_msvc}" -fcoverage-mapping)
set(_msvc_no_g1 TRUE)
if(_msvc_g1)
    set(_msvc_no_g1 FALSE)
endif()
set(_msvc_no_o0 TRUE)
if(_msvc_o0)
    set(_msvc_no_o0 FALSE)
endif()
# clang-cl rejects -g1 ("unknown argument") and warns on -O0; both break any
# dependency built with -Werror.
expect("GNU driver gets line tables (-g1)" ${_gnu_g1})
expect("GNU driver gets -O0" ${_gnu_o0})
expect("clang-cl does not get -g1" ${_msvc_no_g1})
expect("clang-cl does not get -O0" ${_msvc_no_o0})
expect("clang-cl still gets coverage mapping" ${_msvc_map})

if(_failures GREATER 0)
    message(FATAL_ERROR "${_failures} instrumentation flag check(s) failed")
endif()
