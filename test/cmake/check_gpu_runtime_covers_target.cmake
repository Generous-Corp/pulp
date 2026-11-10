# Drives pulp_gpu_runtime_covers_target() through the macOS target shapes Pulp
# configures: gpu-runtime's renderer lock covers macos-arm64 only, so a build
# with no arm64 slice (the release CLI's x86_64 cross-compile) must keep Pulp's
# own device sources instead of failing the lock's Dawn digest check.
# RUNTIME_MODULE, when given, also includes PulpGpuRuntime.cmake itself for an
# x86_64-only target and asserts it keeps Pulp's own device sources; that path
# returns before any fetch, so it needs no network or Skia tree.
#   cmake -DMODULE=<PulpGpuRuntimeRecord.cmake> [-DRUNTIME_MODULE=<PulpGpuRuntime.cmake>]
#         -P check_gpu_runtime_covers_target.cmake
cmake_minimum_required(VERSION 3.24)
if(MODE STREQUAL "module")
    set(APPLE 1)
    set(PULP_SKIA_PROVIDES_DAWN ON)
    set(SKIA_DIR "${CMAKE_CURRENT_LIST_DIR}/no-skia-tree")
    set(CMAKE_OSX_ARCHITECTURES "${ARCHS}")
    set(CMAKE_SYSTEM_PROCESSOR "${PROC}")
    include("${RUNTIME_MODULE}")
    message(STATUS "module-result PULP_HAS_GPU_RUNTIME=${PULP_HAS_GPU_RUNTIME}")
    return()
endif()
include("${MODULE}")
# <name>|<CMAKE_OSX_ARCHITECTURES, ',' for ';'>|<CMAKE_SYSTEM_PROCESSOR>|<TRUE or FALSE>
set(_cases
    "host-arm64||arm64|TRUE"
    "explicit-arm64|arm64|arm64|TRUE"
    "universal|arm64,x86_64|arm64|TRUE"
    "universal-reversed|x86_64,arm64|x86_64|TRUE"
    "cross-x86_64-on-arm64|x86_64|arm64|FALSE"
    "host-x86_64||x86_64|FALSE"
    "explicit-x86_64-on-x86_64|x86_64|x86_64|FALSE"
    "upper-case-arm64|ARM64|x86_64|TRUE"
    "aarch64-processor||aarch64|TRUE")
set(_failures 0)
foreach(_case IN LISTS _cases)
    string(REPLACE "|" ";" _fields "${_case}")
    list(LENGTH _fields _count)
    if(NOT _count EQUAL 4)
        message(FATAL_ERROR "malformed case '${_case}'")
    endif()
    list(GET _fields 0 _name)
    list(GET _fields 1 _archs)
    list(GET _fields 2 _processor)
    list(GET _fields 3 _expect)
    string(REPLACE "," ";" _archs "${_archs}")
    unset(_covered)
    pulp_gpu_runtime_covers_target("${_archs}" "${_processor}" _covered)
    if(NOT DEFINED _covered)
        message(SEND_ERROR "${_name}: the result variable was not set")
        math(EXPR _failures "${_failures} + 1")
    elseif(NOT _covered STREQUAL _expect)
        message(SEND_ERROR "${_name}: expected ${_expect}, got ${_covered} "
            "(archs='${_archs}', processor='${_processor}')")
        math(EXPR _failures "${_failures} + 1")
    endif()
endforeach()
if(RUNTIME_MODULE)
    foreach(_shape "x86_64|arm64" "|x86_64")
        string(REPLACE "|" ";" _shape_fields "${_shape}")
        list(GET _shape_fields 0 _archs)
        list(GET _shape_fields 1 _processor)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -DMODE=module -DRUNTIME_MODULE=${RUNTIME_MODULE}
                "-DARCHS=${_archs}" -DPROC=${_processor} -P "${CMAKE_CURRENT_LIST_FILE}"
            RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
        if(NOT _result EQUAL 0 OR NOT "${_out}${_err}" MATCHES "module-result PULP_HAS_GPU_RUNTIME=FALSE")
            message(SEND_ERROR "PulpGpuRuntime.cmake with archs='${_archs}' processor='${_processor}' "
                "did not keep Pulp's own device sources (rc=${_result}):\n${_out}${_err}")
            math(EXPR _failures "${_failures} + 1")
        endif()
    endforeach()
endif()
if(_failures GREATER 0)
    message(FATAL_ERROR "${_failures} gpu-runtime target-coverage case(s) failed")
endif()
list(LENGTH _cases _total)
message(STATUS "gpu-runtime target coverage: ${_total} cases pass")
