# Evaluate tools/cmake/PulpGpuRuntime.cmake with real CMake and record what it does.
#
# tools/deps/audit.py runs this with `cmake -P` and asserts on the record, so
# comments, quoting, escapes, constant conditions, return() and variable
# expansion all mean exactly what CMake makes them mean.
#
#   cmake -DPULP_DRIVER_ROOT=<repo> [-DPULP_DRIVER_MODULE=<pin file>] -DPULP_DRIVER_RECORD=<file>
#         -DPULP_DRIVER_FETCHED_VERSION=<x.y.z>
#         [-DPULP_DRIVER_PREFIX_MODE=ON] [-DPULP_VELLUM_LOCK_FILE=<lock>]
#         -P gpu_runtime_driver.cmake
#
# The module runs with the macOS gpu-runtime conditions it gates on. These
# commands are replaced by recorders:
#   include()                    PulpVellumLock.cmake and PulpGpuRuntimeRecord.cmake
#                                under <repo>/tools/cmake are really included;
#                                every other include is recorded and skipped.
#                                After the lock reader, a `lock` line records the
#                                PULP_VELLUM_LOCK_FILE it read.
#   FetchContent_Declare()       recorded.
#   FetchContent_MakeAvailable() recorded, with any FetchContent source override
#                                in effect; sets <name>_SOURCE_DIR to a fixed
#                                stand-in directory.
#   get_directory_property()     recorded; PROJECT_VERSION of the stand-in
#                                gpu-runtime directory is PULP_DRIVER_FETCHED_VERSION.
#   find_package()               recorded; finds nothing.
#   pulp_check_gpu_runtime_record(), pulp_check_gpu_runtime_archive()
#                                recorded (the prefix integrity checks have their
#                                own tests).
#   message(FATAL_ERROR|SEND_ERROR)
#                                recorded, then stops as CMake would. (Where it
#                                was called from comes from CMake's own trace.)
#
# Each record line is the command name and its arguments, tab-separated, each
# argument hex-encoded so empty arguments, semicolons and newlines survive.
cmake_minimum_required(VERSION 3.24)

foreach(_pulp_driver_required PULP_DRIVER_ROOT PULP_DRIVER_RECORD PULP_DRIVER_FETCHED_VERSION)
    if(NOT DEFINED ${_pulp_driver_required})
        message(FATAL_ERROR "gpu_runtime_driver: ${_pulp_driver_required} is required")
    endif()
endforeach()
set(_pulp_driver_cmake_dir "${PULP_DRIVER_ROOT}/tools/cmake")
if(NOT DEFINED PULP_DRIVER_MODULE)
    set(PULP_DRIVER_MODULE "${_pulp_driver_cmake_dir}/PulpGpuRuntime.cmake")
endif()
set(_pulp_driver_fetched_dir "/pulp-driver/vellum_gpu_runtime")
file(WRITE "${PULP_DRIVER_RECORD}" "")

# Records the calling command. A macro, so it reads the caller's ARGV<n>
# variables through ${ARGV${_i}}; the caller sets _pulp_driver_argc first.
macro(_pulp_driver_capture name)
    set(_pulp_driver_line "${name}")
    if(_pulp_driver_argc GREATER 0)
        math(EXPR _pulp_driver_last "${_pulp_driver_argc} - 1")
        foreach(_pulp_driver_i RANGE 0 ${_pulp_driver_last})
            string(HEX "${ARGV${_pulp_driver_i}}" _pulp_driver_hex)
            string(APPEND _pulp_driver_line "\t${_pulp_driver_hex}")
        endforeach()
    endif()
    file(APPEND "${PULP_DRIVER_RECORD}" "${_pulp_driver_line}\n")
endmacro()

function(FetchContent_Declare)
    set(_pulp_driver_argc ${ARGC})
    _pulp_driver_capture(FetchContent_Declare)
endfunction()

function(FetchContent_MakeAvailable)
    set(_pulp_driver_argc ${ARGC})
    _pulp_driver_capture(FetchContent_MakeAvailable)
    foreach(_pulp_driver_name ${ARGV})
        string(TOUPPER "${_pulp_driver_name}" _pulp_driver_upper)
        set(_pulp_driver_argc 3)
        set(ARGV0 "FETCHCONTENT_SOURCE_DIR_${_pulp_driver_upper}")
        set(ARGV1 "${FETCHCONTENT_SOURCE_DIR_${_pulp_driver_upper}}")
        set(ARGV2 "${FETCHCONTENT_FULLY_DISCONNECTED}")
        _pulp_driver_capture(FetchContentOverrides)
        set(${_pulp_driver_name}_SOURCE_DIR "${_pulp_driver_fetched_dir}" PARENT_SCOPE)
    endforeach()
endfunction()

function(get_directory_property out)
    set(_pulp_driver_argc ${ARGC})
    _pulp_driver_capture(get_directory_property)
    set(_pulp_driver_value "")
    if(ARGC EQUAL 5 AND ARGV1 STREQUAL "DIRECTORY"
       AND ARGV2 STREQUAL "${_pulp_driver_fetched_dir}/gpu-runtime"
       AND ARGV3 STREQUAL "DEFINITION" AND ARGV4 STREQUAL "PROJECT_VERSION")
        set(_pulp_driver_value "${PULP_DRIVER_FETCHED_VERSION}")
    endif()
    set(${out} "${_pulp_driver_value}" PARENT_SCOPE)
endfunction()

function(find_package)
    set(_pulp_driver_argc ${ARGC})
    _pulp_driver_capture(find_package)
    set(${ARGV0}_FOUND FALSE PARENT_SCOPE)
endfunction()

function(message)
    if(ARGV0 STREQUAL "FATAL_ERROR" OR ARGV0 STREQUAL "SEND_ERROR")
        set(_pulp_driver_mode "${ARGV0}")
        set(_pulp_driver_argc ${ARGC})
        _pulp_driver_capture(message)
        _message(FATAL_ERROR "gpu_runtime_driver: stopped by message(${_pulp_driver_mode})")
    endif()
endfunction()

# Defined after PulpGpuRuntimeRecord.cmake is included, replacing its checks.
# Kept as text because a function written inside a macro body would have its
# ${ARGC} substituted by the macro.
set(_pulp_driver_prefix_stubs [=[
function(pulp_check_gpu_runtime_record)
    set(_pulp_driver_argc ${ARGC})
    _pulp_driver_capture(pulp_check_gpu_runtime_record)
endfunction()
function(pulp_check_gpu_runtime_archive)
    set(_pulp_driver_argc ${ARGC})
    _pulp_driver_capture(pulp_check_gpu_runtime_archive)
endfunction()
]=])

function(_pulp_driver_record_lock)
    set(_pulp_driver_argc ${ARGC})
    _pulp_driver_capture(lock)
endfunction()

# A function, so its argument is exactly what CMake passed (a macro would
# re-expand an escaped \${...}). A passed-through file runs here, and every
# variable it set or changed is copied to the including scope, as include()
# would leave it.
function(include)
    set(_pulp_driver_argc ${ARGC})
    _pulp_driver_capture(include)
    set(_pulp_driver_target "")
    if(ARGC EQUAL 1)
        get_filename_component(_pulp_driver_target "${ARGV0}" REALPATH)
    endif()
    if(NOT _pulp_driver_target STREQUAL "${_pulp_driver_cmake_dir}/PulpVellumLock.cmake"
       AND NOT _pulp_driver_target STREQUAL "${_pulp_driver_cmake_dir}/PulpGpuRuntimeRecord.cmake")
        return()
    endif()
    get_cmake_property(_pulp_driver_before VARIABLES)
    foreach(_pulp_driver_var IN LISTS _pulp_driver_before)
        set("_pulp_driver_old_${_pulp_driver_var}" "${${_pulp_driver_var}}")
    endforeach()
    _include("${_pulp_driver_target}")
    if(_pulp_driver_target STREQUAL "${_pulp_driver_cmake_dir}/PulpVellumLock.cmake")
        _pulp_driver_record_lock("${PULP_VELLUM_LOCK_FILE}")
    else()
        cmake_language(EVAL CODE "${_pulp_driver_prefix_stubs}")
    endif()
    get_cmake_property(_pulp_driver_after VARIABLES)
    foreach(_pulp_driver_var IN LISTS _pulp_driver_after)
        if(_pulp_driver_var MATCHES "^(_pulp_driver_|ARG[VCN])")
            continue()
        endif()
        if(NOT DEFINED "_pulp_driver_old_${_pulp_driver_var}"
           OR NOT "${${_pulp_driver_var}}" STREQUAL "${_pulp_driver_old_${_pulp_driver_var}}")
            set(${_pulp_driver_var} "${${_pulp_driver_var}}" PARENT_SCOPE)
        endif()
    endforeach()
    foreach(_pulp_driver_var IN LISTS _pulp_driver_before)
        if(NOT DEFINED ${_pulp_driver_var})
            unset(${_pulp_driver_var} PARENT_SCOPE)
        endif()
    endforeach()
endfunction()

# The conditions PulpGpuRuntime.cmake gates on for a macOS gpu-runtime build.
set(APPLE TRUE)
set(PULP_IOS FALSE)
set(PULP_SKIA_PROVIDES_DAWN TRUE)
set(SKIA_DIR "/pulp-driver/skia")
set(PULP_BENCHMARK OFF)
set(PULP_DAWN_PROCS_HOST_OWNED OFF)
if(PULP_DRIVER_PREFIX_MODE)
    set(PULP_GPU_RUNTIME_PREFIX "/pulp-driver/prefix" CACHE PATH "" FORCE)
    set(PULP_GPU_RUNTIME_ARCHIVE "/pulp-driver/archive.tar.gz" CACHE FILEPATH "" FORCE)
endif()

_include("${PULP_DRIVER_MODULE}")
file(APPEND "${PULP_DRIVER_RECORD}" "end\n")
