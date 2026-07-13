# PulpGpuWasm.cmake — reusable build helper for the Skia-free WebGPU DSP module.
#
# Compiles render::GpuCompute + the submit/collect WebGpuConvolver into a
# WebAssembly module that runs inside a DedicatedWorker and drives the browser's
# WebGPU implementation through Dawn's Emscripten port (--use-port=emdawnwebgpu).
# Requires the Emscripten toolchain (configure with `emcmake cmake ...`); under
# any other toolchain this file is a no-op.
#
# WHY A CURATED SOURCE LIST INSTEAD OF LINKING pulp::render:
# The same reason PulpWam.cmake curates a DSP subset. `pulp-render` is
# desktop-shaped — it publicly links pulp::view / pulp::canvas / Skia / Dawn's
# NATIVE library, and its GPU-surface sources are built against a windowing
# system. None of that belongs in, or builds in, a browser worker. GpuCompute
# itself needs none of it: it is WebGPU compute over webgpu_cpp.h, and
# emdawnwebgpu supplies exactly that API on top of navigator.gpu.
#
# THE SKIA-FREE PROPERTY IS LOAD-BEARING, NOT COSMETIC. A DSP worker that drags
# in Skia/Graphite pulls tens of megabytes of renderer into a module whose job is
# an FFT, and re-couples the audio path to the UI's GPU stack. NEVER pass
# -DSKIA_DIR to a target declared here, never add a canvas/view/gpu_surface
# source, and never add -sASYNCIFY (the point of the async readback is that it
# does not need it) or -pthread (the module is a single-threaded worker).
# test/cmake/gpu_audio_web_tests.cmake enforces the property on the emitted
# artifacts.
#
# First build downloads the emdawnwebgpu package from
# github.com/google/dawn/releases into EM_CACHE. Expect that once.

include_guard(GLOBAL)

if(NOT EMSCRIPTEN)
    message(STATUS "PulpGpuWasm: GPU DSP module skipped (not an Emscripten build).")
    return()
endif()

# Repo root = two levels up from tools/cmake/.
get_filename_component(_PULP_GPU_DSP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(_PULP_GPU_DSP_INCLUDES
    ${_PULP_GPU_DSP_ROOT}/core/platform/include
    ${_PULP_GPU_DSP_ROOT}/core/runtime/include
    ${_PULP_GPU_DSP_ROOT}/core/audio/include
    ${_PULP_GPU_DSP_ROOT}/core/signal/include
    ${_PULP_GPU_DSP_ROOT}/core/render/include
    ${_PULP_GPU_DSP_ROOT}/core/gpu_audio/include
)

# The whole module: the WGSL compute kernels + their WebGPU plumbing, and the
# never-blocking convolver. Nothing else — GpuCompute's only other Pulp
# dependency is pulp/runtime/log.hpp, which is header-only, so NO runtime TU is
# linked (core/runtime/src/runtime.cpp would pull choc + the background-job pool
# in for symbols this module never calls). If this list ever needs a third core
# source, that is a signal the module is growing a dependency it should not have.
set(_PULP_GPU_DSP_SOURCES
    ${_PULP_GPU_DSP_ROOT}/core/render/src/gpu_compute.cpp
    ${_PULP_GPU_DSP_ROOT}/core/gpu_audio/src/web/web_gpu_convolver.cpp
)

# PULP_HAS_DAWN selects GpuCompute's real implementation (the alternative is the
# null stub that returns nullptr). It is a FACT about this lane, not a per-target
# option.
#
# There is deliberately no second define selecting the BROWSER arm: gpu_compute.cpp
# keys that off `__EMSCRIPTEN__`, which the compiler defines here and only here, so
# a lane macro would be a second source of truth that could disagree with the
# toolchain. When you go looking for the browser arm (plain wgpuCreateInstance, no
# dawn::native proc table, no blocking event-queue pump), grep __EMSCRIPTEN__.
set(_PULP_GPU_DSP_DEFINES PULP_HAS_DAWN=1)

# pulp_add_gpu_dsp_module(<Name>
#     ENTRY    <entry.cpp>          # required: the pulp_gpu_* C ABI entry point
#     [SOURCES <extra .cpp> ...]
#     [INCLUDES <dir> ...]
#     [DEFINES <MACRO=1> ...])
#
# Emits <Name>.js + <Name>.wasm — an ES-module factory the worker imports. Not
# SINGLE_FILE: this module is loaded in a DedicatedWorker, which has fetch and
# async compile (unlike an AudioWorkletGlobalScope, which is why the WAM lane
# embeds its wasm).
function(pulp_add_gpu_dsp_module NAME)
    cmake_parse_arguments(ARG "" "ENTRY" "SOURCES;INCLUDES;DEFINES;EXPORTS" ${ARGN})
    if(NOT ARG_ENTRY)
        message(FATAL_ERROR "pulp_add_gpu_dsp_module(${NAME}): ENTRY <entry.cpp> is required.")
    endif()
    if(NOT ARG_EXPORTS)
        message(FATAL_ERROR "pulp_add_gpu_dsp_module(${NAME}): EXPORTS <_fn ...> is required.")
    endif()

    add_executable(${NAME} ${_PULP_GPU_DSP_SOURCES} ${ARG_ENTRY} ${ARG_SOURCES})
    target_include_directories(${NAME} PRIVATE
        ${_PULP_GPU_DSP_INCLUDES} ${ARG_INCLUDES})
    target_compile_definitions(${NAME} PRIVATE
        ${_PULP_GPU_DSP_DEFINES} ${ARG_DEFINES})
    target_compile_features(${NAME} PRIVATE cxx_std_20)
    # --use-port is needed at COMPILE time (webgpu_cpp.h) and at LINK time (the
    # JS glue that binds it to navigator.gpu).
    target_compile_options(${NAME} PRIVATE --use-port=emdawnwebgpu -O2)

    list(JOIN ARG_EXPORTS "','" _exports_joined)
    set(_exports "['_malloc','_free','${_exports_joined}']")

    set(_link
        "--use-port=emdawnwebgpu"
        "-sEXPORTED_FUNCTIONS=${_exports}"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','stringToNewUTF8','HEAPF32','HEAPU8']"
        "-sMODULARIZE=1"
        "-sEXPORT_ES6=1"
        "-sALLOW_MEMORY_GROWTH=1"
        # Emscripten >= 6.0 backs a growable heap with a RESIZABLE ArrayBuffer
        # (wasmMemory.toResizableBuffer()) whenever the engine offers one.
        # Chrome ships that API but its TextDecoder still REJECTS views of a
        # resizable buffer ("The provided ArrayBuffer value must not be
        # resizable"), so every UTF8ToString of a string longer than 16 bytes
        # throws — which is every WGSL source and every Dawn error message.
        # MEASURED: the GPU lane died in startGpuLane on exactly that throw.
        # Pin the classic detach-and-re-view growth path until Chrome's
        # TextDecoder accepts resizable buffers.
        "-sGROWABLE_ARRAYBUFFERS=0"
        "-sENVIRONMENT=web,worker"
        "--no-entry"
        "-O2"
    )
    string(JOIN " " _link_str ${_link})
    set_target_properties(${NAME} PROPERTIES
        OUTPUT_NAME "${NAME}"
        SUFFIX ".js"
        LINK_FLAGS "${_link_str}")
endfunction()
