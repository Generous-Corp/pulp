# Configure-time contract for the ARM64EC SIMD fallback detection.

include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/PulpSimdSupport.cmake")

set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64EC)
set(WIN32 FALSE)
set(CMAKE_GENERATOR_PLATFORM "")
pulp_simd_target_is_arm64ec(_compiler_detected)
if(NOT _compiler_detected)
    message(FATAL_ERROR "ARM64EC compiler identity was not detected")
endif()

set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64)
set(WIN32 TRUE)
set(CMAKE_GENERATOR_PLATFORM ARM64EC)
pulp_simd_target_is_arm64ec(_generator_detected)
if(NOT _generator_detected)
    message(FATAL_ERROR "ARM64EC generator platform was not detected")
endif()

set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64)
set(CMAKE_GENERATOR_PLATFORM ARM64)
pulp_simd_target_is_arm64ec(_arm64_detected)
if(_arm64_detected)
    message(FATAL_ERROR "plain ARM64 was misclassified as ARM64EC")
endif()

set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID x64)
set(CMAKE_GENERATOR_PLATFORM x64)
pulp_simd_target_is_arm64ec(_x64_detected)
if(_x64_detected)
    message(FATAL_ERROR "x64 was misclassified as ARM64EC")
endif()
