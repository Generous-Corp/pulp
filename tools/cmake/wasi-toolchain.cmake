# WASI SDK CMake toolchain for building WCLAP plugins
# Based on wasi-sdk's toolchain, customized for Pulp WCLAP targets.
#
# Usage:
#   cmake -S . -B build-wclap \
#     -DCMAKE_TOOLCHAIN_FILE=tools/cmake/wasi-toolchain.cmake
#
# Prerequisites:
#   - wasi-sdk installed (default: /opt/wasi-sdk)
#   - Set WASI_SDK_PREFIX env var if installed elsewhere
#
# References:
#   - https://github.com/WebAssembly/wasi-sdk
#   - https://github.com/WebCLAP
#   - https://github.com/geraintluff/signalsmith-clap-cpp

# Find wasi-sdk
if(NOT DEFINED WASI_SDK_PREFIX)
    if(DEFINED ENV{WASI_SDK_PREFIX})
        set(WASI_SDK_PREFIX "$ENV{WASI_SDK_PREFIX}")
    elseif(EXISTS "/opt/wasi-sdk")
        set(WASI_SDK_PREFIX "/opt/wasi-sdk")
    else()
        message(FATAL_ERROR "wasi-sdk not found. Set WASI_SDK_PREFIX or install to /opt/wasi-sdk")
    endif()
endif()

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

# Compiler paths
set(CMAKE_C_COMPILER "${WASI_SDK_PREFIX}/bin/clang")
set(CMAKE_CXX_COMPILER "${WASI_SDK_PREFIX}/bin/clang++")
set(CMAKE_AR "${WASI_SDK_PREFIX}/bin/llvm-ar")
set(CMAKE_RANLIB "${WASI_SDK_PREFIX}/bin/llvm-ranlib")
set(CMAKE_LINKER "${WASI_SDK_PREFIX}/bin/wasm-ld")

# Sysroot
set(CMAKE_SYSROOT "${WASI_SDK_PREFIX}/share/wasi-sysroot")

# Target triple — wasm32-wasi with threads support
set(WASI_TARGET "wasm32-wasi-threads")
set(CMAKE_C_COMPILER_TARGET "${WASI_TARGET}")
set(CMAKE_CXX_COMPILER_TARGET "${WASI_TARGET}")

# Compile flags
set(CMAKE_C_FLAGS_INIT "-fno-exceptions")
set(CMAKE_CXX_FLAGS_INIT "-fno-exceptions -fno-rtti")

# WebAssembly-specific flags for WCLAP
# - Export function table (required by WebCLAP hosts)
# - Shared memory for threading support
# - Reactor mode (no main(), entry via clap_entry)
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-Wl,--export-table -Wl,--growable-table -mexec-model=reactor"
)

# Don't try to run test executables
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Mark as cross-compiling
set(CMAKE_CROSSCOMPILING TRUE)

# WCLAP-specific defines
add_compile_definitions(
    PULP_WCLAP=1
    __wasi__=1
)
