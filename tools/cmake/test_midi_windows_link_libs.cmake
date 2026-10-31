# How core/midi links the WindowsApp umbrella import library on Windows.
#
# WindowsApp also imports the OS icu.dll. As an ordinary link input it can win
# ICU symbols ahead of Skia's bundled ICU, and lld-link then reports duplicate
# symbols (ures_getByKey, u_charsToUChars) for any module that pulls a Skia ICU
# object too. On MSVC-style linkers it must therefore be a default library,
# which is searched only after every library on the command line. The current
# MSVC path intentionally omits WindowsApp entirely and uses runtimeobject.
#
# Script mode (`cmake -P`). core/midi/CMakeLists.txt is evaluated in a child
# `cmake -P` of this file with the project-only commands stubbed to record the
# link libraries and link options they receive.

cmake_minimum_required(VERSION 3.20)

if(DEFINED PULP_MIDI_PROBE_MSVC)
    foreach(_cmd add_library add_executable target_sources target_compile_definitions
                 target_include_directories set_target_properties find_package)
        cmake_language(EVAL CODE "function(${_cmd})\nendfunction()")
    endforeach()
    # Arguments are joined with `|` so a recorded call stays one list element.
    function(target_link_libraries)
        list(JOIN ARGN "|" _args)
        message(STATUS "link-libraries:|${_args}|")
    endfunction()
    function(target_link_options)
        list(JOIN ARGN "|" _args)
        message(STATUS "link-options:|${_args}|")
    endfunction()
    unset(APPLE)
    unset(IOS)
    unset(UNIX)
    unset(ANDROID)
    set(WIN32 1)
    set(MSVC ${PULP_MIDI_PROBE_MSVC})
    set(CMAKE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
    set(CMAKE_CURRENT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../core/midi")
    include(${CMAKE_CURRENT_LIST_DIR}/../../core/midi/CMakeLists.txt)
    return()
endif()

set(_failures 0)

function(link_lines_for msvc out)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -DPULP_MIDI_PROBE_MSVC=${msvc} -P ${CMAKE_CURRENT_LIST_FILE}
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
    string(REGEX MATCHALL "link-(libraries|options):[^\n]*" _lines "${_out}${_err}")
    if(NOT _rc EQUAL 0 OR NOT _lines)
        message(FATAL_ERROR "probe for MSVC=${msvc} failed (${_rc}):\n${_out}${_err}")
    endif()
    set(${out} "${_lines}" PARENT_SCOPE)
endfunction()

function(expect what holds)
    if(${holds})
        message(STATUS "ok: ${what}")
    else()
        message(STATUS "FAIL: ${what}")
        math(EXPR _n "${_failures} + 1")
        set(_failures ${_n} PARENT_SCOPE)
    endif()
endfunction()

link_lines_for(1 _msvc)
set(_default_lib FALSE)
set(_input_lib FALSE)
set(_runtimeobject FALSE)
foreach(_line IN LISTS _msvc)
    if(_line MATCHES "^link-options:\\|pulp-midi\\|INTERFACE\\|/DEFAULTLIB:WindowsApp\\.lib\\|$")
        set(_default_lib TRUE)
    endif()
    if(_line MATCHES "^link-libraries:.*\\|WindowsApp\\|")
        set(_input_lib TRUE)
    endif()
    if(_line MATCHES "^link-libraries:.*\\|runtimeobject\\|")
        set(_runtimeobject TRUE)
    endif()
endforeach()
if(_default_lib)
    set(_not_default_lib FALSE)
else()
    set(_not_default_lib TRUE)
endif()
expect("MSVC-style linkers do not add WindowsApp as an INTERFACE /DEFAULTLIB" _not_default_lib)
if(_input_lib)
    set(_not_input FALSE)
else()
    set(_not_input TRUE)
endif()
expect("MSVC-style linkers never get WindowsApp as a link input" _not_input)
expect("runtimeobject stays an ordinary link input" _runtimeobject)

link_lines_for(0 _gnu)
set(_gnu_input FALSE)
foreach(_line IN LISTS _gnu)
    if(_line MATCHES "^link-libraries:.*\\|WindowsApp\\|")
        set(_gnu_input TRUE)
    endif()
endforeach()
expect("non-MSVC Windows linkers still link WindowsApp" _gnu_input)

if(_failures GREATER 0)
    message(FATAL_ERROR "${_failures} midi Windows link expectation(s) failed")
endif()
