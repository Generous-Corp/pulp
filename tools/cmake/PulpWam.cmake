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
#
# NOTE: the wam_* C ABI entry point is NOT in this shared object library. It is
# added per-target instead, because a single-plugin build links wam_entry.cpp
# (one global WamProcessorBridge) while a rack build links wam_chain_entry.cpp
# (one global WamChainBridge) — the two define the SAME C symbols and must never
# be linked together. Both compile against wam_adapter.cpp, which lives here.
set(_PULP_WAM_CORE_SOURCES
    ${_PULP_WAM_ROOT}/core/runtime/src/runtime.cpp
    ${_PULP_WAM_ROOT}/core/runtime/src/identity.cpp
    ${_PULP_WAM_ROOT}/core/state/src/store.cpp
    ${_PULP_WAM_ROOT}/core/state/src/state_migration.cpp
    ${_PULP_WAM_ROOT}/core/events/src/event_loop.cpp
    ${_PULP_WAM_ROOT}/core/format/src/processor_f64.cpp
    ${_PULP_WAM_ROOT}/core/format/src/registry.cpp
    ${_PULP_WAM_ROOT}/core/format/src/wasm/wam_adapter.cpp
    ${_PULP_WAM_ROOT}/core/format/src/wasm/headless_defaults.cpp
)

# The two mutually-exclusive wam_* C ABI entry points (see note above).
set(_PULP_WAM_SINGLE_ENTRY ${_PULP_WAM_ROOT}/core/format/src/wasm/wam_entry.cpp)
set(_PULP_WAM_CHAIN_ENTRY  ${_PULP_WAM_ROOT}/core/format/src/wasm/wam_chain_entry.cpp)

add_library(pulp-wam-dsp OBJECT ${_PULP_WAM_CORE_SOURCES})
target_include_directories(pulp-wam-dsp PUBLIC ${_PULP_WAM_INCLUDES})
target_compile_options(pulp-wam-dsp PRIVATE -fno-exceptions -fno-rtti -O2)

# The wam_* C symbols every plugin entry point exports.
# THE wam_* ABI IS LISTED IN FOUR PLACES THAT MUST STAY IN SYNC:
#   1. core/format/src/wasm/wam_entry.cpp        (the C definitions)
#      AND core/format/src/wasm/wam_chain_entry.cpp (the rack's parallel copy,
#      which exports the SAME symbol names against a WamChainBridge)
#   2. this EXPORTED_FUNCTIONS list              (Emscripten export table)
#   3. core/format/src/wasm/wam-runtime.mjs      (makeBridge methods)
#   4. core/format/src/wasm/wam-processor.js     (moduleExports adapter)
# Adding/removing a wam_* function means editing all four (and both entry TUs).
# A rack introduces NO new export: it reuses this exact ABI, so the allowlist,
# makeBridge, and moduleExports are shared unchanged between plugins and racks.
set(_PULP_WAM_EXPORTED_FUNCTIONS
    "['_malloc','_free','_wam_init','_wam_process','_wam_set_param','_wam_get_param','_wam_midi','_wam_midi_sysex','_wam_midi_out_drain','_wam_reset','_wam_prepare','_wam_latency_samples','_wam_set_transport','_wam_descriptor','_wam_parameters','_wam_state_size','_wam_read_state','_wam_write_state']")

# Shared link-flag + output-naming logic for a WAM target (plugin or rack). The
# SINGLE_FILE form BASE64-embeds the wasm with synchronous compilation (required
# inside an AudioWorkletGlobalScope — no fetch / no async compile there); the
# default separate-wasm form is for the Node runner + export inspection.
function(_pulp_wam_apply_link_flags TARGET NAME IS_SINGLE_FILE)
    set(_link
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXPORTED_FUNCTIONS=${_PULP_WAM_EXPORTED_FUNCTIONS}"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','HEAPF32','HEAPU8']"
        "--no-entry"
        "-O2"
    )
    if(IS_SINGLE_FILE)
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
    set_target_properties(${TARGET} PROPERTIES
        OUTPUT_NAME "${NAME}"
        SUFFIX ".js"
        LINK_FLAGS "${_link_str}")
endfunction()

# pulp_add_wam_plugin(<Name>
#     ENTRY    <entry.cpp>              # required: the wam_* C entry point
#     [SOURCES <extra .cpp> ...]        # optional: extra plugin DSP sources
#     [INCLUDES <dir> ...]              # optional: extra include dirs (plugin headers)
#     [SINGLE_FILE])                    # optional: BASE64-embed wasm for AudioWorklet
#
# Emits <Name>.js (+ <Name>.wasm unless SINGLE_FILE). Links wam_entry.cpp (one
# global WamProcessorBridge — a single plugin per module).
function(pulp_add_wam_plugin NAME)
    cmake_parse_arguments(ARG "SINGLE_FILE;NATIVE_EDITOR" "ENTRY" "SOURCES;INCLUDES" ${ARGN})
    if(NOT ARG_ENTRY)
        message(FATAL_ERROR "pulp_add_wam_plugin(${NAME}): ENTRY <entry.cpp> is required.")
    endif()

    add_executable(${NAME}-wam
        $<TARGET_OBJECTS:pulp-wam-dsp>
        ${_PULP_WAM_SINGLE_ENTRY}
        ${ARG_ENTRY}
        ${ARG_SOURCES}
    )
    target_include_directories(${NAME}-wam PRIVATE ${_PULP_WAM_INCLUDES} ${ARG_INCLUDES})
    target_compile_options(${NAME}-wam PRIVATE -fno-exceptions -fno-rtti -O2)

    _pulp_wam_apply_link_flags(${NAME}-wam ${NAME} "${ARG_SINGLE_FILE}")

    # Emit a web-build report (<Name>.web-build.json) documenting what the web
    # build does with this plugin's UI: the generated-controls strategy, the
    # parameter binding targets a design-import UI maps to by id, and — when
    # NATIVE_EDITOR is declared — that the plugin's native editor is replaced by
    # generated controls in the headless web build. Needs the separate .wasm, so
    # it is skipped for SINGLE_FILE (worklet) targets, and skipped (with a status
    # message) when node is unavailable.
    if(NOT ARG_SINGLE_FILE)
        find_program(_PULP_WAM_NODE node)
        if(_PULP_WAM_NODE)
            set(_report_args
                "${_PULP_WAM_NODE}"
                "${_PULP_WAM_ROOT}/core/format/src/wasm/wam_build_report.mjs"
                --wasm "$<TARGET_FILE_DIR:${NAME}-wam>/${NAME}.wasm"
                --out "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.web-build.json")
            if(ARG_NATIVE_EDITOR)
                list(APPEND _report_args --native-editor)
            endif()
            add_custom_command(TARGET ${NAME}-wam POST_BUILD
                COMMAND ${_report_args}
                COMMENT "Writing ${NAME}.web-build.json"
                VERBATIM)
        else()
            message(STATUS "PulpWam: node not found — skipping ${NAME}.web-build.json report.")
        endif()
    endif()
endfunction()

# pulp_add_wam_rack(<Name>
#     ENTRY    <rack.cpp>               # required: defines pulp_wam_make_chain()
#     [SOURCES <extra .cpp> ...]        # optional: extra DSP sources
#     [INCLUDES <dir> ...]              # optional: extra include dirs (plugin headers)
#     [SINGLE_FILE])                    # optional: BASE64-embed wasm for AudioWorklet
#
# Compiles an in-worklet CHAIN RACK: N processors in ONE wasm module, driven
# through the identical wam_* C ABI as a single plugin. The ONLY build-level
# difference from pulp_add_wam_plugin is that it links wam_chain_entry.cpp (one
# global WamChainBridge) INSTEAD of wam_entry.cpp — both keep building, and the
# two entry TUs are never linked together (they define the same C symbols). The
# rack's ENTRY TU supplies the stage list:
#
#     std::vector<std::unique_ptr<pulp::format::Processor>> pulp_wam_make_chain();
#
# Emits <Name>.js (+ <Name>.wasm unless SINGLE_FILE). No per-plugin web-build
# report is generated (that surface is about a single plugin's editor mapping).
function(pulp_add_wam_rack NAME)
    cmake_parse_arguments(ARG "SINGLE_FILE" "ENTRY" "SOURCES;INCLUDES" ${ARGN})
    if(NOT ARG_ENTRY)
        message(FATAL_ERROR "pulp_add_wam_rack(${NAME}): ENTRY <rack.cpp> is required.")
    endif()

    add_executable(${NAME}-wam
        $<TARGET_OBJECTS:pulp-wam-dsp>
        ${_PULP_WAM_CHAIN_ENTRY}
        ${ARG_ENTRY}
        ${ARG_SOURCES}
    )
    target_include_directories(${NAME}-wam PRIVATE ${_PULP_WAM_INCLUDES} ${ARG_INCLUDES})
    target_compile_options(${NAME}-wam PRIVATE -fno-exceptions -fno-rtti -O2)

    _pulp_wam_apply_link_flags(${NAME}-wam ${NAME} "${ARG_SINGLE_FILE}")
endfunction()
