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

# SDK25 remains the default profile. The isolated sample-region profile uses
# SDK33's standard Wasm EH libraries throughout the C++ object closure.
option(PULP_SAMPLE_REGION_WEB "Build the isolated sample-region EH/RTTI web profile" OFF)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PULP_SAMPLE_REGION_WEB WASI_SDK_PREFIX)
if(PULP_SAMPLE_REGION_WEB)
    if(NOT EXISTS "${WASI_SDK_PREFIX}/VERSION")
        message(FATAL_ERROR "Sample-region WebCLAP requires WASI SDK 33.0")
    endif()
    file(READ "${WASI_SDK_PREFIX}/VERSION" _pulp_wasi_version)
    string(STRIP "${_pulp_wasi_version}" _pulp_wasi_version)
    if(NOT _pulp_wasi_version MATCHES "^33[.]0([+].*)?$")
        message(FATAL_ERROR "Sample-region WebCLAP requires pinned WASI SDK 33.0, got ${_pulp_wasi_version}")
    endif()
    set(WASI_TARGET "wasm32-wasip1-threads")
    if(NOT EXISTS "${CMAKE_SYSROOT}/lib/${WASI_TARGET}/eh/libunwind.a")
        message(FATAL_ERROR "Selected WASI SDK lacks the threaded exception runtime")
    endif()
    set(CMAKE_C_FLAGS_INIT "-pthread -D_WASI_EMULATED_SIGNAL -fno-lto")
    set(CMAKE_CXX_FLAGS_INIT "-pthread -D_WASI_EMULATED_SIGNAL -frtti -fwasm-exceptions -mllvm -wasm-use-legacy-eh=false -fno-lto")
else()
    set(WASI_TARGET "wasm32-wasi-threads")
    set(CMAKE_C_FLAGS_INIT "-fno-exceptions -pthread -D_WASI_EMULATED_SIGNAL")
    set(CMAKE_CXX_FLAGS_INIT "-fno-exceptions -fno-rtti -pthread -D_WASI_EMULATED_SIGNAL")
endif()
set(CMAKE_C_COMPILER_TARGET "${WASI_TARGET}")
set(CMAKE_CXX_COMPILER_TARGET "${WASI_TARGET}")

# WebAssembly-specific link flags for a WebCLAP module:
# - reactor mode: no _start; the host drives the module through clap_entry.
# - export/growable function table: WebCLAP hosts call plugin callbacks through it.
# - shared + imported + exported memory with a fixed max: the threaded target
#   requires a shared memory, and WebCLAP hosts supply it as an import.
# - wasi-emulated-signal: provides the omitted signal symbols.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-mexec-model=reactor -lwasi-emulated-signal \
     -Wl,--export-table -Wl,--growable-table \
     -Wl,--shared-memory -Wl,--import-memory -Wl,--export-memory \
     -Wl,--max-memory=1073741824"
)

if(PULP_SAMPLE_REGION_WEB)
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -fwasm-exceptions -lunwind -fno-lto")
endif()

# Don't try to run test executables
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Mark as cross-compiling
set(CMAKE_CROSSCOMPILING TRUE)

# WCLAP-specific defines
add_compile_definitions(
    PULP_WCLAP=1
    __wasi__=1
)
