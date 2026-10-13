# Contract test for target-aware Windows bundle architecture naming.

include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/PulpWindowsTargetArch.cmake")

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

# The target wins over the host processor when cross-compiling x64 on ARM64.
set(CMAKE_SYSTEM_PROCESSOR ARM64)
pulp_windows_target_arch(_cross_x64)
if(NOT _cross_x64 STREQUAL "x86_64-win")
    message(FATAL_ERROR "x64 cross-target selected '${_cross_x64}', expected x86_64-win")
endif()

# An unknown target must remain an explicit x86 bundle classification rather
# than inheriting the host processor and silently claiming an ARM provider.
set(CMAKE_GENERATOR_PLATFORM unknown)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID unknown)
pulp_windows_target_arch(_unknown)
if(NOT _unknown STREQUAL "x86-win")
    message(FATAL_ERROR "unknown target selected '${_unknown}', expected x86-win")
endif()

message(STATUS "PASS: Windows target architecture contract")
