# Every macOS target shape forwards its device layer to gpu-runtime: arm64,
# x86_64 (native or cross-compiled from arm64) and universal. Each one resolves
# the universal skia-build asset that gpu-runtime's renderer lock pins, so no
# architecture may opt out of gpu-runtime and fall back to Pulp's own device
# sources.
#
# Each shape includes PulpGpuRuntime.cmake with PULP_GPU_RUNTIME_PREFIX naming
# an empty directory. Reaching gpu-runtime there means stopping on the missing
# gpu-runtime-artifact.json, so the check needs no network or Skia tree. A shape
# that returned early instead would finish with PULP_HAS_GPU_RUNTIME=FALSE.
# The control case sets PULP_USE_GPU_RUNTIME=OFF and must return early, which
# proves the probe can tell the two outcomes apart.
#   cmake -DRUNTIME_MODULE=<PulpGpuRuntime.cmake> -DWORK_DIR=<dir>
#         -P check_gpu_runtime_apple_targets.cmake
cmake_minimum_required(VERSION 3.24)
if(MODE STREQUAL "module")
    set(APPLE 1)
    set(PULP_SKIA_PROVIDES_DAWN ON)
    set(SKIA_DIR "${CMAKE_CURRENT_LIST_DIR}/no-skia-tree")
    set(PULP_USE_GPU_RUNTIME "${USE}" CACHE BOOL "" FORCE)
    set(PULP_GPU_RUNTIME_PREFIX "${PREFIX}" CACHE PATH "" FORCE)
    string(REPLACE "," ";" CMAKE_OSX_ARCHITECTURES "${ARCHS}")
    set(CMAKE_SYSTEM_PROCESSOR "${PROC}")
    include("${RUNTIME_MODULE}")
    message(STATUS "module-result PULP_HAS_GPU_RUNTIME=${PULP_HAS_GPU_RUNTIME}")
    return()
endif()
if(NOT RUNTIME_MODULE OR NOT WORK_DIR)
    message(FATAL_ERROR "RUNTIME_MODULE and WORK_DIR are required")
endif()
set(_prefix "${WORK_DIR}/gpu-runtime-apple-targets-empty-prefix")
file(REMOVE_RECURSE "${_prefix}")
file(MAKE_DIRECTORY "${_prefix}")
# <name>|<CMAKE_OSX_ARCHITECTURES, ',' for ';'>|<CMAKE_SYSTEM_PROCESSOR>|<PULP_USE_GPU_RUNTIME>|<reached or skipped>
set(_cases
    "host-arm64||arm64|ON|reached"
    "explicit-arm64|arm64|arm64|ON|reached"
    "universal|arm64,x86_64|arm64|ON|reached"
    "universal-reversed|x86_64,arm64|x86_64|ON|reached"
    "cross-x86_64-on-arm64|x86_64|arm64|ON|reached"
    "host-x86_64||x86_64|ON|reached"
    "explicit-x86_64-on-x86_64|x86_64|x86_64|ON|reached"
    "control-runtime-off|x86_64|arm64|OFF|skipped")
set(_failures 0)
foreach(_case IN LISTS _cases)
    string(REPLACE "|" ";" _fields "${_case}")
    list(LENGTH _fields _count)
    if(NOT _count EQUAL 5)
        message(FATAL_ERROR "malformed case '${_case}'")
    endif()
    list(GET _fields 0 _name)
    list(GET _fields 1 _archs)
    list(GET _fields 2 _processor)
    list(GET _fields 3 _use)
    list(GET _fields 4 _expect)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DMODE=module "-DRUNTIME_MODULE=${RUNTIME_MODULE}"
            "-DARCHS=${_archs}" "-DPROC=${_processor}" "-DUSE=${_use}" "-DPREFIX=${_prefix}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    # CMake word-wraps FATAL_ERROR text at a width that depends on the prefix
    # path's length, so collapse whitespace before matching the message.
    string(REGEX REPLACE "[ \t\r\n]+" " " _log "${_out}${_err}")
    if(NOT _result EQUAL 0 AND _log MATCHES "has no gpu-runtime-artifact.json")
        set(_got reached)
    elseif(_result EQUAL 0 AND _log MATCHES "module-result PULP_HAS_GPU_RUNTIME=FALSE")
        set(_got skipped)
    else()
        set(_got "unrecognized (rc=${_result})")
    endif()
    if(NOT _got STREQUAL _expect)
        message(SEND_ERROR "${_name}: expected gpu-runtime ${_expect}, got ${_got} "
            "(archs='${_archs}', processor='${_processor}', PULP_USE_GPU_RUNTIME=${_use}):\n${_log}")
        math(EXPR _failures "${_failures} + 1")
    endif()
endforeach()
file(REMOVE_RECURSE "${_prefix}")
if(_failures GREATER 0)
    message(FATAL_ERROR "${_failures} gpu-runtime Apple-target case(s) failed")
endif()
list(LENGTH _cases _total)
message(STATUS "gpu-runtime Apple targets: ${_total} cases pass")
