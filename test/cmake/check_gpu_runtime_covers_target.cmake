# Drives pulp_gpu_runtime_covers_target() through the macOS target shapes Pulp
# configures: gpu-runtime's renderer lock covers macos-arm64 only, so a build
# with no arm64 slice (the release CLI's x86_64 cross-compile) must keep Pulp's
# own device sources instead of failing the lock's Dawn digest check.
#   cmake -DMODULE=<PulpGpuRuntimeRecord.cmake> -P check_gpu_runtime_covers_target.cmake
cmake_minimum_required(VERSION 3.24)
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
    list(GET _fields 0 _name)
    if(_count EQUAL 4)
        list(GET _fields 1 _archs)
        list(GET _fields 2 _processor)
        list(GET _fields 3 _expect)
    else()
        set(_archs "")
        list(GET _fields 1 _processor)
        list(GET _fields 2 _expect)
    endif()
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
if(_failures GREATER 0)
    message(FATAL_ERROR "${_failures} gpu-runtime target-coverage case(s) failed")
endif()
list(LENGTH _cases _total)
message(STATUS "gpu-runtime target coverage: ${_total} cases pass")
