# Contract test for target-aware Windows bundle architecture naming.

include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/PulpWindowsTargetArch.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/PulpWindowsSkiaProvider.cmake")

# Fail-closed modes for the Skia auto-fetch resolver: an ARM64EC/ARM64X target
# with no staged archive, and an architecture the resolver cannot serve.
if(DEFINED PULP_SKIA_RESOLVE_NEGATIVE)
    set(WIN32 TRUE)
    set(CMAKE_GENERATOR_PLATFORM "${PULP_SKIA_RESOLVE_NEGATIVE}")
    set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "${PULP_SKIA_RESOLVE_NEGATIVE}")
    pulp_windows_skia_autofetch_resolve(_plat _suffix _staged "${PULP_SKIA_EMPTY_ROOT}")
    message(FATAL_ERROR "resolver accepted ${PULP_SKIA_RESOLVE_NEGATIVE} with nothing staged")
endif()

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

# Skia auto-fetch selection, through the same helpers PulpDependencies.cmake
# calls. Each case sets only the simulated toolchain: the helpers compute the
# target architecture themselves.
string(RANDOM LENGTH 8 _skia_tag)
set(_skia_tmp "${CMAKE_CURRENT_BINARY_DIR}/windows-skia-autofetch-${_skia_tag}")
set(_sdk "${_skia_tmp}/sdk/external/skia-build")
set(_checkout "${_skia_tmp}/checkout/external/skia-build")
set(_empty "${_skia_tmp}/empty/external/skia-build")
file(MAKE_DIRECTORY "${_empty}")

function(_expect_equal what actual expected)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${what}: got '${actual}', expected '${expected}'")
    endif()
endfunction()

# ARM64EC with a producer archive staged in the installed SDK: no auto-fetch
# slice, and the staged root is reused (the checkout has none).
file(MAKE_DIRECTORY "${_sdk}/build/win-gpu/lib/Release/arm64ec")
file(TOUCH "${_sdk}/build/win-gpu/lib/Release/arm64ec/skia.lib")
set(CMAKE_GENERATOR_PLATFORM ARM64EC)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64)
pulp_windows_skia_autofetch_resolve(_ec_plat _ec_suffix _ec_staged "${_sdk}" "${_checkout}")
_expect_equal("ARM64EC auto-fetch slice" "${_ec_plat}" "")
_expect_equal("ARM64EC staged root" "${_ec_staged}" "${_sdk}")

# x64 with a checkout-local archive in the x64 slice, then in the legacy flat
# layout: both are reused instead of forcing an auto-fetch.
set(CMAKE_GENERATOR_PLATFORM x64)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID x64)
pulp_windows_skia_autofetch_resolve(_x64_plat _x64_suffix _x64_staged "${_sdk}" "${_checkout}")
_expect_equal("x64 auto-fetch slice" "${_x64_plat}" "windows-x64")
_expect_equal("x64 staged root" "${_x64_staged}" "")
file(MAKE_DIRECTORY "${_checkout}/build/win-gpu/lib/Release/x64")
file(TOUCH "${_checkout}/build/win-gpu/lib/Release/x64/skia.lib")
pulp_windows_local_skia(_x64_local "${_checkout}")
_expect_equal("x64 checkout-local archive" "${_x64_local}"
    "${_checkout}/build/win-gpu/lib/Release/x64/skia.lib")
file(REMOVE "${_checkout}/build/win-gpu/lib/Release/x64/skia.lib")
file(TOUCH "${_checkout}/build/win-gpu/lib/Release/skia.lib")
pulp_windows_local_skia(_x64_flat "${_checkout}")
_expect_equal("x64 legacy flat archive" "${_x64_flat}"
    "${_checkout}/build/win-gpu/lib/Release/skia.lib")
file(REMOVE "${_checkout}/build/win-gpu/lib/Release/skia.lib")

# ARM64 must not reuse an x64-only checkout archive.
file(TOUCH "${_checkout}/build/win-gpu/lib/Release/x64/skia.lib")
set(CMAKE_GENERATOR_PLATFORM ARM64)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64)
pulp_windows_local_skia(_arm64_local "${_checkout}")
_expect_equal("ARM64 against an x64-only checkout" "${_arm64_local}" "")

foreach(_negative ARM64EC unknown)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DPULP_SKIA_RESOLVE_NEGATIVE=${_negative}"
            "-DPULP_SKIA_EMPTY_ROOT=${_empty}" -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE _skia_negative_rc
        OUTPUT_VARIABLE _skia_negative_out
        ERROR_VARIABLE _skia_negative_err)
    if(_negative STREQUAL "ARM64EC")
        set(_expected "cannot auto-fetch Windows arm64ec Skia")
    else()
        set(_expected "cannot select a prebuilt Windows Skia slice")
    endif()
    if(_skia_negative_rc EQUAL 0 OR NOT _skia_negative_err MATCHES "${_expected}")
        message(FATAL_ERROR "Skia resolver negative ${_negative}: ${_skia_negative_out}${_skia_negative_err}")
    endif()
endforeach()
file(REMOVE_RECURSE "${_skia_tmp}")

message(STATUS "PASS: Windows target architecture contract")
