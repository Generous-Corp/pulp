# Contract test for target-aware Windows bundle architecture naming.

include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/PulpWindowsTargetArch.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/PulpWindowsSkiaProvider.cmake")

if(DEFINED PULP_WGPU_NEGATIVE)
    set(WIN32 TRUE)
    set(CMAKE_GENERATOR_PLATFORM "${PULP_WGPU_NEGATIVE}")
    pulp_windows_wgpu_prebuilt_arch(_unreachable)
    message(FATAL_ERROR "unsupported ${PULP_WGPU_NEGATIVE} target was accepted")
endif()

set(WIN32 TRUE)
set(CMAKE_SYSTEM_PROCESSOR ARM64)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64)
set(CMAKE_GENERATOR_PLATFORM ARM64EC)
pulp_windows_target_arch(_arm64ec)
if(NOT _arm64ec STREQUAL "arm64ec-win")
    message(FATAL_ERROR "ARM64EC selected '${_arm64ec}', expected arm64ec-win")
endif()

set(CMAKE_GENERATOR_PLATFORM ARM64)
pulp_windows_target_arch(_arm64)
if(NOT _arm64 STREQUAL "arm64-win")
    message(FATAL_ERROR "ARM64 selected '${_arm64}', expected arm64-win")
endif()

set(CMAKE_GENERATOR_PLATFORM ARM64X)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64X)
pulp_windows_target_arch(_arm64x)
if(NOT _arm64x STREQUAL "arm64x-win")
    message(FATAL_ERROR "ARM64X selected '${_arm64x}', expected arm64x-win")
endif()

set(CMAKE_GENERATOR_PLATFORM x64)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID x64)
pulp_windows_target_arch(_x64)
if(NOT _x64 STREQUAL "x86_64-win")
    message(FATAL_ERROR "x64 selected '${_x64}', expected x86_64-win")
endif()
pulp_windows_wgpu_prebuilt_arch(_x64_wgpu)
if(NOT _x64_wgpu STREQUAL "x86_64")
    message(FATAL_ERROR "x64 wgpu selected '${_x64_wgpu}', expected x86_64")
endif()
pulp_windows_skia_autofetch_slice(_x64_skia _x64_suffix)
if(NOT _x64_skia STREQUAL "windows-x64" OR NOT _x64_suffix STREQUAL "-x64")
    message(FATAL_ERROR "x64 Skia fetch selected '${_x64_skia}' '${_x64_suffix}'")
endif()

# The target wins over the host processor when cross-compiling x64 on ARM64.
set(CMAKE_SYSTEM_PROCESSOR ARM64)
pulp_windows_target_arch(_cross_x64)
if(NOT _cross_x64 STREQUAL "x86_64-win")
    message(FATAL_ERROR "x64 cross-target selected '${_cross_x64}', expected x86_64-win")
endif()
set(CMAKE_GENERATOR_PLATFORM "")
pulp_windows_wgpu_prebuilt_arch(_ninja_cross_x64_wgpu)
if(NOT _ninja_cross_x64_wgpu STREQUAL "x86_64")
    message(FATAL_ERROR "Ninja ARM-hosted x64 wgpu selected '${_ninja_cross_x64_wgpu}'")
endif()
pulp_windows_skia_autofetch_slice(_ninja_cross_x64_skia _ninja_cross_x64_suffix)
if(NOT _ninja_cross_x64_skia STREQUAL "windows-x64"
        OR NOT _ninja_cross_x64_suffix STREQUAL "-x64")
    message(FATAL_ERROR "Ninja ARM-hosted x64 Skia fetch selected '${_ninja_cross_x64_skia}'")
endif()
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64)
pulp_windows_wgpu_prebuilt_arch(_arm64_wgpu)
if(NOT _arm64_wgpu STREQUAL "aarch64")
    message(FATAL_ERROR "ARM64 wgpu selected '${_arm64_wgpu}', expected aarch64")
endif()
pulp_windows_skia_autofetch_slice(_arm64_skia _arm64_suffix)
if(NOT _arm64_skia STREQUAL "windows-arm64" OR NOT _arm64_suffix STREQUAL "-arm64")
    message(FATAL_ERROR "ARM64 Skia fetch selected '${_arm64_skia}' '${_arm64_suffix}'")
endif()

foreach(_unsupported ARM64EC ARM64X)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPULP_WGPU_NEGATIVE=${_unsupported}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE _negative_rc
        OUTPUT_VARIABLE _negative_output
        ERROR_VARIABLE _negative_error)
    if(_negative_rc EQUAL 0 OR NOT _negative_error MATCHES "GPU configure is fail-closed")
        message(FATAL_ERROR "${_unsupported} negative failed: ${_negative_output}${_negative_error}")
    endif()
endforeach()

# An unknown target must remain an explicit x86 bundle classification rather
# than inheriting the host processor and silently claiming an ARM provider.
set(CMAKE_GENERATOR_PLATFORM unknown)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID unknown)
pulp_windows_target_arch(_unknown)
if(NOT _unknown STREQUAL "x86-win")
    message(FATAL_ERROR "unknown target selected '${_unknown}', expected x86-win")
endif()

message(STATUS "PASS: Windows target architecture contract")
