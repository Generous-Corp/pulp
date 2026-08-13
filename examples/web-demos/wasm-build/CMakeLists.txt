# WASM build for Pulp web demo plugins.
# Compiles PulpGain, PulpPluck, PulpChorus, and SuperConvolver to WebAssembly
# WAMv2 modules.
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

# Run the installed-SDK registered-content consumer against the exact object
# closure every WAM plugin links. Keeping the existing consumer as the entry
# point makes its fixture, seed, exact notes, and committed semantic hash the
# shared native/WASM oracle without introducing a second source inventory.
add_executable(pulp-wam-registered-chord-renderer
    $<TARGET_OBJECTS:pulp-wam-dsp>
    ${PULP_ROOT}/examples/timeline-sdk-consumer/registered_chord_renderer.cpp)
target_include_directories(pulp-wam-registered-chord-renderer PRIVATE ${_PULP_WAM_INCLUDES})
target_compile_definitions(pulp-wam-registered-chord-renderer PRIVATE
    PULP_WASM=1 PULP_HEADLESS=1 PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1)
target_compile_options(pulp-wam-registered-chord-renderer PRIVATE
    -fno-exceptions -fno-rtti -O2)
set_target_properties(pulp-wam-registered-chord-renderer PROPERTIES
    SUFFIX ".js"
    LINK_FLAGS "-sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node -O2")

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

# SuperConvolver — the convolution reverb from examples/super-convolver, built
# headless. PULP_WASM/PULP_HEADLESS (defined unconditionally by PulpWam.cmake,
# same as the WebCLAP lane) compile out the desktop-only surface: the GPU
# multi-room engine, the file-backed IR loader + native file dialog, and the
# native GPU editor. What remains is the CPU PartitionedConvolver against the
# built-in synthetic IR — SuperConvolver's DEFAULT engine on the desktop too, so
# the browser module runs the same CPU convolution DSP the desktop default runs;
# the GPU engine and disk IR loading are not present in the browser at all. Both
# delivery shapes are built — the separate .wasm for superconvolver_runner.mjs,
# the SINGLE_FILE one for the worklet.
pulp_add_wam_plugin(SuperConvolver
    ENTRY    ${CMAKE_CURRENT_SOURCE_DIR}/super_convolver_wasm.cpp
    INCLUDES ${PULP_ROOT}/examples/super-convolver
    NATIVE_EDITOR)

pulp_add_wam_plugin(SuperConvolverWorklet SINGLE_FILE
    ENTRY    ${CMAKE_CURRENT_SOURCE_DIR}/super_convolver_wasm.cpp
    INCLUDES ${PULP_ROOT}/examples/super-convolver)

# An in-worklet RACK (pulp-pluck → pulp-gain) built through pulp_add_wam_rack.
# This is the committed CI coverage for the chain path — it compiles
# wam_chain_entry.cpp + WamChainBridge and is exercised by wam_rack_runner.mjs.
pulp_add_wam_rack(PulpPluckGainRack
    ENTRY    ${CMAKE_CURRENT_SOURCE_DIR}/pluck_gain_rack_wasm.cpp
    INCLUDES ${PULP_ROOT}/examples/pulp-pluck ${PULP_ROOT}/examples/pulp-gain)
