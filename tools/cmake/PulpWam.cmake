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
include(${CMAKE_CURRENT_LIST_DIR}/PulpPortableWav.cmake)
pulp_resolve_portable_wav(
    "${_PULP_WAM_ROOT}"
    _PULP_WAM_WAV_SOURCES
    _PULP_WAM_WAV_INCLUDES)

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
    ${_PULP_WAM_ROOT}/core/timebase/include
    ${_PULP_WAM_ROOT}/core/timeline/include
    ${_PULP_WAM_ROOT}/core/playback/include
    ${_PULP_WAM_ROOT}/core/state/include
    ${_PULP_WAM_ROOT}/core/audio/include
    ${_PULP_WAM_ROOT}/core/midi/include
    ${_PULP_WAM_ROOT}/core/events/include
    ${_PULP_WAM_ROOT}/core/format/include
    ${_PULP_WAM_ROOT}/core/signal/include
    ${_PULP_WAM_WAV_INCLUDES}
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
    ${_PULP_WAM_ROOT}/core/timebase/src/compiled_meter_map.cpp
    ${_PULP_WAM_ROOT}/core/timebase/src/compiled_tempo_map.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/assets.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/automation_curve.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/automation_document_internal.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/automation_lane.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/command.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/document_session.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/identity_directory.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/id_remap.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/journal.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/model.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/schema_json.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/schema_json_canonical.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/schema_json_parser.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/schema_json_preflight.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/schema_registry.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/serialize_automation_decode.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/serialize_decode.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/serialize_encode.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/snapshot_equivalence.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/structural_registry_validation.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/track.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/track_schema_migrations.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/transaction.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/transaction_automation_internal.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/transaction_reduction_support.cpp
    ${_PULP_WAM_ROOT}/core/timeline/src/undo.cpp
    ${_PULP_WAM_ROOT}/core/playback/src/audio_renderer.cpp
    ${_PULP_WAM_ROOT}/core/playback/src/automation_cursor.cpp
    ${_PULP_WAM_ROOT}/core/playback/src/automation_program.cpp
    ${_PULP_WAM_ROOT}/core/playback/src/compile_executor.cpp
    ${_PULP_WAM_ROOT}/core/playback/src/note_renderer.cpp
    ${_PULP_WAM_ROOT}/core/playback/src/program.cpp
    ${_PULP_WAM_ROOT}/core/playback/src/program_compiler.cpp
    ${_PULP_WAM_ROOT}/core/playback/src/stable_renderer_shell.cpp
    ${_PULP_WAM_ROOT}/core/playback/src/track_automation_program.cpp
    ${_PULP_WAM_ROOT}/core/playback/src/transport.cpp
    ${_PULP_WAM_WAV_SOURCES}
    ${_PULP_WAM_ROOT}/core/runtime/src/runtime.cpp
    ${_PULP_WAM_ROOT}/core/runtime/src/identity.cpp
    ${_PULP_WAM_ROOT}/core/state/src/store.cpp
    ${_PULP_WAM_ROOT}/core/state/src/state_migration.cpp
    ${_PULP_WAM_ROOT}/core/events/src/event_loop.cpp
    ${_PULP_WAM_ROOT}/core/format/src/processor_f64.cpp
    ${_PULP_WAM_ROOT}/core/format/src/registry.cpp
    ${_PULP_WAM_ROOT}/core/format/src/plugin_state_io.cpp
    ${_PULP_WAM_ROOT}/core/format/src/wasm/wam_adapter.cpp
    ${_PULP_WAM_ROOT}/core/format/src/wasm/headless_defaults.cpp
)

# The two mutually-exclusive wam_* C ABI entry points (see note above).
set(_PULP_WAM_SINGLE_ENTRY ${_PULP_WAM_ROOT}/core/format/src/wasm/wam_entry.cpp)
set(_PULP_WAM_CHAIN_ENTRY  ${_PULP_WAM_ROOT}/core/format/src/wasm/wam_chain_entry.cpp)

add_library(pulp-wam-dsp OBJECT ${_PULP_WAM_CORE_SOURCES})
target_compile_features(pulp-wam-dsp PUBLIC cxx_std_20)
target_include_directories(pulp-wam-dsp PUBLIC ${_PULP_WAM_INCLUDES})
# PULP_WASM / PULP_HEADLESS are FACTS about this lane, not per-target options:
# this file hard-returns unless EMSCRIPTEN (see the guard above), and a WAM
# module is always a headless AudioWorklet DSP module with no editor, no file
# I/O, and no GPU. They are therefore defined here (and in
# _pulp_wam_apply_lane_defines for the executables) rather than left to each
# call site to remember — the same unconditional gating pulp_add_wclap applies.
# Keeping them symmetric with the WebCLAP lane is what makes "same header,
# bit-identical DSP across WAM and WebCLAP" true by construction instead of by
# convention.
target_compile_definitions(pulp-wam-dsp PUBLIC
    PULP_WASM=1 PULP_HEADLESS=1 PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1)
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
    "['_malloc','_free','_wam_init','_wam_process','_wam_set_param','_wam_get_param','_wam_midi','_wam_param_epoch','_wam_read_param_values','_wam_midi_sysex','_wam_midi_out_drain','_wam_reset','_wam_prepare','_wam_latency_samples','_wam_set_transport','_wam_descriptor','_wam_parameters','_wam_state_size','_wam_read_state','_wam_write_state']")

# Shared link-flag + output-naming logic for a WAM target (plugin or rack). The
# SINGLE_FILE form embeds the wasm INSIDE the emitted .js and compiles it
# synchronously — required inside an AudioWorkletGlobalScope, which has no fetch
# and no async compile; the default separate-wasm form is for the Node runner +
# export inspection. (The embedding encoding is an emcc implementation detail and
# has changed under us: base64 through emcc 4.0.17, a raw UTF-8-safe encoding
# from 4.0.18 on. Nothing here depends on which — only that the wasm ships inside
# the .js — but the emsdk pin in .github/workflows/web-plugins.yml is exact so a
# floating toolchain cannot change it silently.)
function(_pulp_wam_apply_lane_defines TARGET)
    # See the note on pulp-wam-dsp: the WAM lane is by definition wasm + headless.
    target_compile_definitions(${TARGET} PRIVATE PULP_WASM=1 PULP_HEADLESS=1)
endfunction()

function(_pulp_wam_apply_link_flags TARGET NAME IS_SINGLE_FILE)
    set(_link
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXPORTED_FUNCTIONS=${_PULP_WAM_EXPORTED_FUNCTIONS}"
        "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','HEAPF32','HEAPU8']"
        "--no-entry"
        "-O2"
    )
    if(IS_SINGLE_FILE)
        # AudioWorklet DSP module: embed the wasm in the .js (SINGLE_FILE) and
        # emit an ES-module factory (MODULARIZE+EXPORT_ES6). The processor module
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
#     [DEFINES  <MACRO=1> ...]          # optional: extra compile definitions
#     [SINGLE_FILE])                    # optional: embed the wasm for AudioWorklet
#
# Emits <Name>.js (+ <Name>.wasm unless SINGLE_FILE). Links wam_entry.cpp (one
# global WamProcessorBridge — a single plugin per module).
#
# PULP_WASM=1 and PULP_HEADLESS=1 are ALWAYS defined (see the note above the
# pulp-wam-dsp target): they gate out every desktop-only surface a plugin header
# may carry (native editor, file I/O, GPU engine) behind `#if !PULP_HEADLESS` /
# `#if !PULP_WASM`, exactly as pulp_add_wclap does for the WebCLAP lane. A plugin
# with no such surface (PulpGain, PulpPluck) is unaffected. DEFINES is for
# genuine per-plugin extras only — never for the lane identity.
function(pulp_add_wam_plugin NAME)
    cmake_parse_arguments(ARG "SINGLE_FILE;NATIVE_EDITOR" "ENTRY" "SOURCES;INCLUDES;DEFINES" ${ARGN})
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
    _pulp_wam_apply_lane_defines(${NAME}-wam)
    if(ARG_DEFINES)
        target_compile_definitions(${NAME}-wam PRIVATE ${ARG_DEFINES})
    endif()

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
    _pulp_wam_apply_lane_defines(${NAME}-wam)

    _pulp_wam_apply_link_flags(${NAME}-wam ${NAME} "${ARG_SINGLE_FILE}")
endfunction()
