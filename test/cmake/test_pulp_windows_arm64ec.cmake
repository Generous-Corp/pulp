# Contract tests for the project-wide Windows ARM64EC compiler switch.
# These run on any host through cmake -P; no Windows compiler is needed.

include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/PulpWindowsArm64Ec.cmake")

# Simulate the CMake variables that a Visual Studio ARM64EC configure exposes.
set(WIN32 TRUE)
set(PULP_WINDOWS_ARM64EC_CONTRACT_TEST TRUE)
set(CMAKE_GENERATOR_PLATFORM ARM64EC)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64EC)
set(CMAKE_C_FLAGS "")
set(CMAKE_CXX_FLAGS "")
pulp_windows_target_is_arm64ec(_ec)
if(NOT _ec)
    message(FATAL_ERROR "ARM64EC target was not detected")
endif()
pulp_windows_apply_arm64ec_flags()
if(NOT CMAKE_C_FLAGS MATCHES "(^|[ ;])/arm64EC([ ;]|$)")
    message(FATAL_ERROR "ARM64EC was not applied to C flags: '${CMAKE_C_FLAGS}'")
endif()
if(NOT CMAKE_CXX_FLAGS MATCHES "(^|[ ;])/arm64EC([ ;]|$)")
    message(FATAL_ERROR "ARM64EC was not applied to CXX flags: '${CMAKE_CXX_FLAGS}'")
endif()

# ARM64X is a mixed image format. It must not force every translation unit to
# EC, because native ARM64 sources are valid and required in that image.
set(CMAKE_GENERATOR_PLATFORM ARM64X)
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64X)
set(CMAKE_C_FLAGS "")
set(CMAKE_CXX_FLAGS "")
pulp_windows_target_is_arm64ec(_arm64x)
if(_arm64x)
    message(FATAL_ERROR "ARM64X was incorrectly classified as ARM64EC")
endif()
pulp_windows_apply_arm64ec_flags()
if(CMAKE_C_FLAGS MATCHES "/arm64EC" OR CMAKE_CXX_FLAGS MATCHES "/arm64EC")
    message(FATAL_ERROR "ARM64X received a project-wide /arm64EC flag")
endif()

# An explicit compiler flag remains a valid fallback when a generator does not
# expose the target identity.
set(CMAKE_GENERATOR_PLATFORM "")
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64)
set(CMAKE_C_FLAGS "")
set(CMAKE_CXX_FLAGS "/DKEEP /arm64EC")
pulp_windows_target_is_arm64ec(_flag_ec)
if(NOT _flag_ec)
    message(FATAL_ERROR "explicit /arm64EC compiler flag was not detected")
endif()
pulp_windows_apply_arm64ec_flags()
if(NOT CMAKE_C_FLAGS MATCHES "/arm64EC")
    message(FATAL_ERROR "explicit /arm64EC did not propagate to C flags")
endif()

message(STATUS "PASS: Windows ARM64EC compiler contract")
