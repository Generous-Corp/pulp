# pulp-gpu-dsp — the Skia-free WebGPU DSP module that runs in the demo's
# DedicatedWorker.
#
# Build (standalone, the way CI and the demo build it):
#   source ~/emsdk/emsdk_env.sh
#   emcmake cmake -S examples/web-demos/gpu-audio -B build-gpu-dsp \
#       -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-gpu-dsp
#
# Output: build-gpu-dsp/pulp-gpu-dsp.js (ES-module factory) + pulp-gpu-dsp.wasm.
#
# The build logic — the curated Skia-free source list, the emdawnwebgpu port, and
# the link flags — lives in tools/cmake/PulpGpuWasm.cmake. This file only declares
# the entry point and the exported ABI.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(pulp-gpu-dsp LANGUAGES CXX)
endif()

# Reached as a subdirectory of the native tree (examples/web-demos/CMakeLists.txt)
# there is nothing to build: this module exists only under Emscripten, and the
# native tree must not gain a dependency on the emsdk. PulpGpuWasm.cmake would
# no-op anyway; returning here keeps its function undefined-but-uncalled.
if(NOT EMSCRIPTEN)
    return()
endif()

get_filename_component(PULP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include(${PULP_ROOT}/tools/cmake/PulpGpuWasm.cmake)

# The pinned C ABI (see gpu_worker_entry.cpp). Adding or removing an entry point
# means editing THREE places: the entry TU, this list, and the worker JS.
pulp_add_gpu_dsp_module(pulp-gpu-dsp
    ENTRY   ${CMAKE_CURRENT_SOURCE_DIR}/gpu_worker_entry.cpp
    EXPORTS _pulp_gpu_init
            _pulp_gpu_kernel_source
            _pulp_gpu_override_kernel
            _pulp_gpu_prepare
            _pulp_gpu_submit
            _pulp_gpu_poll
            _pulp_gpu_stat
            _pulp_gpu_notify_device_lost
            _pulp_gpu_in_buffer
            _pulp_gpu_out_buffer)
