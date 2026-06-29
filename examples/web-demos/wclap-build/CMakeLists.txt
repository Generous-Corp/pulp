# WebCLAP build for Pulp web demo plugins.
# Compiles PulpGain to a WebAssembly CLAP (.wasm) with wasi-sdk.
#
# Build:
#   cd examples/web-demos/wclap-build
#   cmake -S . -B build \
#       -DCMAKE_TOOLCHAIN_FILE=../../../tools/cmake/wasi-toolchain.cmake \
#       -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Output: build/PulpGain.wasm — a WebCLAP module exporting clap_entry +
# malloc/free/cabi_realloc + a growable function table + shared memory.
#
# Validate the module is live (instantiate, run _initialize, resolve clap_entry):
#   node ../../../core/format/src/wasm/wclap_probe.mjs build/PulpGain.wasm
#
# Host it for real — full CLAP lifecycle + audio + parameter control (needs
# WebAssembly.Function, hence the Node flag):
#   node --experimental-wasm-type-reflection \
#     ../../../core/format/src/wasm/wclap_host_runner.mjs build/PulpGain.wasm --gain-db 6
#
# The WebCLAP build logic (portable DSP/adapter source set, link flags, the
# clap_entry/allocator export contract) lives in tools/cmake/PulpWclap.cmake —
# this file only declares which plugins to build. See that helper and
# wasi-toolchain.cmake for why the heavy pulp::format / pulp::state libraries
# are not linked directly and why -fno-exceptions is mandatory.

cmake_minimum_required(VERSION 3.20)
project(pulp-wclap-demos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PULP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
include(${PULP_ROOT}/tools/cmake/PulpWclap.cmake)

pulp_add_wclap(PulpGain
    ENTRY    ${CMAKE_CURRENT_SOURCE_DIR}/pulp_gain_wclap.cpp
    INCLUDES ${PULP_ROOT}/examples/pulp-gain)
