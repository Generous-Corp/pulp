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

# Target triple — wasm32-wasi with threads support. The threaded sysroot is the
# only one whose libc++ tolerates the pthread primitives Pulp's runtime pulls in;
# its libc++abi ships WITHOUT an exception runtime, which is why -fno-exceptions
# below is mandatory, not a size optimization.
set(WASI_TARGET "wasm32-wasi-threads")
set(CMAKE_C_COMPILER_TARGET "${WASI_TARGET}")
set(CMAKE_CXX_COMPILER_TARGET "${WASI_TARGET}")

# Compile flags.
# - -fno-exceptions / -fno-rtti: the threaded sysroot has no EH/RTTI runtime.
# - -pthread: matches the threaded target's TLS / atomics ABI.
# - _WASI_EMULATED_SIGNAL: the runtime references signal symbols the bare
#   wasi-libc omits; the emulation shim (linked below) provides them.
set(CMAKE_C_FLAGS_INIT "-fno-exceptions -pthread -D_WASI_EMULATED_SIGNAL")
set(CMAKE_CXX_FLAGS_INIT "-fno-exceptions -fno-rtti -pthread -D_WASI_EMULATED_SIGNAL")

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

# Don't try to run test executables
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Mark as cross-compiling
set(CMAKE_CROSSCOMPILING TRUE)

# WCLAP-specific defines
add_compile_definitions(
    PULP_WCLAP=1
    __wasi__=1
)
