# PulpWam.cmake — reusable WAMv2 (Web Audio Modules v2) build helper.
#
# Compiles a Pulp Processor into a headless WebAssembly DSP module that exports
# the wam_* C ABI consumed by core/format/src/wasm/wam-plugin.js and
# wam-runtime.mjs. Requires the Emscripten toolchain (configure with
# `emcmake cmake ...`); under any other toolchain this file is a no-op.
#
# WHY A CURATED DSP SUBSET INSTEAD OF LINKING pulp::format / pulp::state:
# A headless WASM DSP module must NOT link the desktop-shaped Pulp libraries.
# `pulp-format` PUBLICLY links pulp::view / pulp::graph / pulp::native-components
# (view pulls the GPU canvas/Skia/Dawn stack) and `pulp-runtime` links mbedTLS +
# an HTTP client — none of which belongs in, or builds in, a browser audio
# worklet. So this helper compiles the portable DSP subset directly. This is the
# SINGLE source of truth for that subset (it previously lived duplicated in the
# example CMakeLists and drifted as core deps grew). A future emcc-portable
# "DSP-core" library split (e.g. pulp-runtime-core without crypto/http,
# pulp-format-core without view/adapters) could replace this list with real
# transitive targets; until then keep _PULP_WAM_CORE_SOURCES in sync with the
# WAM bridge's transitive dependencies.

include_guard(GLOBAL)

if(NOT EMSCRIPTEN)
    message(STATUS "PulpWam: WAM targets skipped (not an Emscripten build).")
    return()
endif()

# Repo root = two levels up from tools/cmake/.
get_filename_component(_PULP_WAM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# choc (header-only, for MIDI). Provided by the parent project's FetchContent or
# an explicit -DPULP_WAM_CHOC_INCLUDE=<dir containing choc/>; otherwise fall back
# to a populated sibling build tree.
if(NOT PULP_WAM_CHOC_INCLUDE)
    foreach(_cand
        "${_PULP_WAM_ROOT}/build/_deps/choc-src"
        "${_PULP_WAM_ROOT}/build-dbg/_deps/choc-src")
        if(EXISTS "${_cand}/choc/audio/choc_MIDI.h")
            set(PULP_WAM_CHOC_INCLUDE "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(NOT PULP_WAM_CHOC_INCLUDE OR NOT EXISTS "${PULP_WAM_CHOC_INCLUDE}/choc/audio/choc_MIDI.h")
    message(FATAL_ERROR
        "PulpWam: choc headers not found. Pass -DPULP_WAM_CHOC_INCLUDE=<dir containing choc/>.")
endif()

set(_PULP_WAM_INCLUDES
    ${_PULP_WAM_ROOT}/core/platform/include
    ${_PULP_WAM_ROOT}/core/runtime/include
    ${_PULP_WAM_ROOT}/core/state/include
    ${_PULP_WAM_ROOT}/core/audio/include
    ${_PULP_WAM_ROOT}/core/midi/include
    ${_PULP_WAM_ROOT}/core/events/include
    ${_PULP_WAM_ROOT}/core/format/include
    ${_PULP_WAM_ROOT}/core/signal/include
    ${PULP_WAM_CHOC_INCLUDE}
)

# Headless DSP subset + WAM bridge — compiled once into an OBJECT library and
# shared across every WAM plugin target.
set(_PULP_WAM_CORE_SOURCES
    ${_PULP_WAM_ROOT}/core/runtime/src/runtime.cpp
    ${_PULP_WAM_ROOT}/core/runtime/src/identity.cpp
    ${_PULP_WAM_ROOT}/core/state/src/store.cpp
    ${_PULP_WAM_ROOT}/core/state/src/state_migration.cpp
    ${_PULP_WAM_ROOT}/core/events/src/event_loop.cpp
    ${_PULP_WAM_ROOT}/core/format/src/registry.cpp
    ${_PULP_WAM_ROOT}/core/format/src/wasm/wam_adapter.cpp
    ${_PULP_WAM_ROOT}/core/format/src/wasm/wam_entry.cpp
    ${_PULP_WAM_ROOT}/core/format/src/wasm/headless_defaults.cpp
)

add_library(pulp-wam-dsp OBJECT ${_PULP_WAM_CORE_SOURCES})
target_include_directories(pulp-wam-dsp PUBLIC ${_PULP_WAM_INCLUDES})
target_compile_options(pulp-wam-dsp PRIVATE -fno-exceptions -fno-rtti -O2)

# The wam_* C symbols every plugin entry point exports.
# THE wam_* ABI IS LISTED IN FOUR PLACES THAT MUST STAY IN SYNC:
#   1. core/format/src/wasm/wam_entry.cpp        (the C definitions)
#   2. this EXPORTED_FUNCTIONS list              (Emscripten export table)
#   3. core/format/src/wasm/wam-runtime.mjs      (makeBridge methods)
#   4. core/format/src/wasm/wam-processor.js     (moduleExports adapter)
# Adding/removing a wam_* function means editing all four.
set(_PULP_WAM_EXPORTED_FUNCTIONS
    "['_malloc','_free','_wam_init','_wam_process','_wam_set_param','_wam_get_param','_wam_midi','_wam_descriptor','_wam_parameters','_wam_state_size','_wam_read_state','_wam_write_state']")

# pulp_add_wam_plugin(<Name>
#     ENTRY    <entry.cpp>              # required: the wam_* C entry point
#     [SOURCES <extra .cpp> ...]        # optional: extra plugin DSP sources
#     [INCLUDES <dir> ...]              # optional: extra include dirs (plugin headers)
#     [SINGLE_FILE])                    # optional: BASE64-embed wasm for AudioWorklet
#
# Emits <Name>.js (+ <Name>.wasm unless SINGLE_FILE). The default separate-wasm
# form is for the Node runner + export inspection. SINGLE_FILE embeds the wasm
# as BASE64 with synchronous compilation, which is required to run inside an
# AudioWorkletGlobalScope (no fetch / no async compile there).
function(pulp_add_wam_plugin NAME)
    cmake_parse_arguments(ARG "SINGLE_FILE" "ENTRY" "SOURCES;INCLUDES" ${ARGN})
    if(NOT ARG_ENTRY)
        message(FATAL_ERROR "pulp_add_wam_plugin(${NAME}): ENTRY <entry.cpp> is required.")
    endif()

    add_executable(${NAME}-wam
        $<TARGET_OBJECTS:pulp-wam-dsp>
        ${ARG_ENTRY}
        ${ARG_SOURCES}
    )
    target_include_directories(${NAME}-wam PRIVATE ${_PULP_WAM_INCLUDES} ${ARG_INCLUDES})
    target_compile_options(${NAME}-wam PRIVATE -fno-exceptions -fno-rtti -O2)

    set(_link
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXPORTED_FUNCTIONS=${_PULP_WAM_EXPORTED_FUNCTIONS}"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','HEAPF32','HEAPU8']"
        "--no-entry"
        "-O2"
    )
    if(ARG_SINGLE_FILE)
        # AudioWorklet DSP module: BASE64-embed the wasm (SINGLE_FILE) and emit
        # an ES-module factory (MODULARIZE+EXPORT_ES6). The processor module
        # imports this factory and awaits it at top level, so Module is fully
        # ready when the AudioWorkletProcessor constructs — a plain global
        # Module is module-scoped under addModule() and never becomes visible.
        list(APPEND _link
            "-sSINGLE_FILE=1"
            "-sMODULARIZE=1"
            "-sEXPORT_ES6=1"
            "-sENVIRONMENT=web,worker")
    else()
        list(APPEND _link
            "-sMODULARIZE=1"
            "-sEXPORT_ES6=1"
            "-sENVIRONMENT=web,worker,node")
    endif()
    string(JOIN " " _link_str ${_link})

    set_target_properties(${NAME}-wam PROPERTIES
        OUTPUT_NAME "${NAME}"
        SUFFIX ".js"
        LINK_FLAGS "${_link_str}")
endfunction()
