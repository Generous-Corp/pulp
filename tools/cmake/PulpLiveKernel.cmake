# PulpLiveKernel.cmake — reusable build helper for the Pulp Live Kernel (S0).
#
# Cloned from tools/cmake/PulpWam.cmake's SINGLE_FILE path, but emits a STANDALONE
# raw .wasm (not a SINGLE_FILE base64 ES-module factory). The live-kernel worklet
# transfers the wasm BYTES in and compiles them SYNCHRONOUSLY inside
# AudioWorkletGlobalScope with `new WebAssembly.Module` (posting a
# WebAssembly.Module is silently dropped in Chrome — WS-C2 DECISION.md §3), so the
# module must be self-contained: STANDALONE_WASM => only wasi_snapshot_preview1
# imports + an internal (exported) memory, no Emscripten JS glue.
#
# This is a NEW, separate build surface: it does NOT touch PulpWam.cmake, the
# wam_* ABI export list, or any wam target. Under any non-Emscripten toolchain it
# is a no-op. The spike itself is built by examples/web-demos/live-kernel-spike/
# build.sh (the same flags); this helper is the productionized CMake form.
#
# Usage:
#   include(PulpLiveKernel)
#   pulp_add_live_kernel(lk_kernel
#     ENTRY   experimental/live_kernel/lk_entry.cpp
#     EXPORTS _lk_init _lk_load_plan _lk_swap _lk_set_param _lk_process
#             _lk_is_fading _lk_active_valid _lk_sample_rate _lk_alloc_count)

include_guard(GLOBAL)

if(NOT EMSCRIPTEN)
    message(STATUS "PulpLiveKernel: targets skipped (not an Emscripten build).")
    return()
endif()

get_filename_component(_PULP_LK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(_PULP_LK_INCLUDES
    ${_PULP_LK_ROOT}/core/signal/include
    ${_PULP_LK_ROOT}/experimental/live_kernel)

# pulp_add_live_kernel(<Name> ENTRY <entry.cpp> [SOURCES ...] [EXPORTS _sym ...])
# Emits <Name>.wasm — a standalone, worklet-instantiable module. malloc/free are
# always exported (the worklet writes the graph blob + reads the output through
# them).
function(pulp_add_live_kernel NAME)
    cmake_parse_arguments(ARG "" "ENTRY" "SOURCES;EXPORTS" ${ARGN})
    if(NOT ARG_ENTRY)
        message(FATAL_ERROR "pulp_add_live_kernel(${NAME}): ENTRY <entry.cpp> is required.")
    endif()

    add_executable(${NAME}-lk ${ARG_ENTRY} ${ARG_SOURCES})
    target_include_directories(${NAME}-lk PRIVATE ${_PULP_LK_INCLUDES})
    target_compile_options(${NAME}-lk PRIVATE -fno-exceptions -fno-rtti -O3)

    set(_exports "'_malloc','_free'")
    foreach(_sym ${ARG_EXPORTS})
        string(APPEND _exports ",'${_sym}'")
    endforeach()

    set(_link
        "-sSTANDALONE_WASM=1" "-sPURE_WASI=0"
        "-sINITIAL_MEMORY=67108864" "-sALLOW_MEMORY_GROWTH=0"
        "-sTOTAL_STACK=1048576"
        "-sEXPORTED_FUNCTIONS=[${_exports}]"
        "--no-entry" "-O3")
    string(JOIN " " _link_str ${_link})
    set_target_properties(${NAME}-lk PROPERTIES
        OUTPUT_NAME "${NAME}"
        SUFFIX ".wasm"
        LINK_FLAGS "${_link_str}")
endfunction()
