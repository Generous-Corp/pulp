# WASM build for Pulp web demo plugins.
# Compiles PulpGain, PulpPluck, PulpChorus to WebAssembly WAMv2 modules.
#
# Build:
#   cd examples/web-demos/wasm-build
#   emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
#       -DPULP_WAM_CHOC_INCLUDE=<dir containing choc/>   # if not auto-located
#   cmake --build build
#
# Output: build/*.js + build/*.wasm for the browser host / Node runner.
#
# The WAMv2 build logic (headless DSP source set, link flags, wam_* export
# table) lives in tools/cmake/PulpWam.cmake — this file only declares which
# plugins to build. See that helper for why the heavy pulp::format / pulp::state
# libraries are not linked directly.

cmake_minimum_required(VERSION 3.20)
project(pulp-wasm-demos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PULP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
include(${PULP_ROOT}/tools/cmake/PulpWam.cmake)

pulp_add_wam_plugin(PulpGain
    ENTRY    ${CMAKE_CURRENT_SOURCE_DIR}/pulp_gain_wasm.cpp
    INCLUDES ${PULP_ROOT}/examples/pulp-gain)

pulp_add_wam_plugin(PulpPluck
    ENTRY    ${CMAKE_CURRENT_SOURCE_DIR}/pulp_pluck_wasm.cpp
    INCLUDES ${PULP_ROOT}/examples/pulp-pluck)

pulp_add_wam_plugin(PulpChorus
    ENTRY    ${CMAKE_CURRENT_SOURCE_DIR}/pulp_chorus_wasm.cpp
    INCLUDES ${PULP_ROOT}/examples/web-demos)

# SINGLE_FILE worklet-delivery build of PulpGain: the wasm is BASE64-embedded
# and compiled synchronously, which is what an AudioWorkletProcessor needs (no
# fetch / no async compile in worklet scope). The separate-wasm PulpGain target
# above stays for the Node runner and export inspection.
pulp_add_wam_plugin(PulpGainWorklet SINGLE_FILE
    ENTRY    ${CMAKE_CURRENT_SOURCE_DIR}/pulp_gain_wasm.cpp
    INCLUDES ${PULP_ROOT}/examples/pulp-gain)
