# PulpWclap.cmake — reusable WebCLAP (.wasm) build helper.
#
# Compiles a Pulp Processor into a WebCLAP module: a CLAP plugin compiled to
# WebAssembly with wasi-sdk. The module exports the standard CLAP `clap_entry`
# global plus the memory allocators a WebCLAP host sandbox calls, and runs in:
#   - native DAWs via a WebCLAP bridge (Wasmtime), or
#   - browsers via a WebCLAP host (AudioWorklet + WebAssembly).
#
# REQUIRES the wasi-sdk toolchain — configure with
#   -DCMAKE_TOOLCHAIN_FILE=tools/cmake/wasi-toolchain.cmake
# Under any other toolchain this file is a no-op (so it can be `include()`d from
# a CMakeLists that is sometimes configured natively).
#
# WHY A CURATED DSP SUBSET INSTEAD OF LINKING pulp::format / pulp::state:
# Same reasoning as PulpWam.cmake — the desktop-shaped Pulp libraries publicly
# pull in the view/GPU stack, mbedTLS, and an HTTP client, none of which builds
# for, or belongs in, a sandboxed WebAssembly plugin. This helper compiles the
# portable subset the CLAP adapter actually needs, and is the SINGLE source of
# truth for that subset. Keep _PULP_WCLAP_CORE_SOURCES in sync with the CLAP
# adapter's transitive dependencies; a stray source that pulls choc's JSON layer
# or a real platform backend will fail to compile under -fno-exceptions and flag
# the drift immediately.
#
# WHY -fno-exceptions IS LOAD-BEARING: the wasi-sdk threaded libc++abi ships
# without an exception runtime. The subset below is exception-clean: the one
# defensive try/catch in plugin_state_io.cpp uses pulp/runtime/exceptions.hpp,
# and the JSON/filesystem-bound native PresetManager is replaced by the headless
# core/state/src/wasm/preset_manager_wasm.cpp stub (a browser sandbox has no
# preset filesystem).

include_guard(GLOBAL)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
    message(STATUS "PulpWclap: WebCLAP targets skipped (not a wasi-sdk build).")
    return()
endif()

# Repo root = two levels up from tools/cmake/.
get_filename_component(_PULP_WCLAP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include(${CMAKE_CURRENT_LIST_DIR}/PulpPortableWav.cmake)
pulp_resolve_portable_wav(
    "${_PULP_WCLAP_ROOT}"
    _PULP_WCLAP_WAV_SOURCES
    _PULP_WCLAP_WAV_INCLUDES)

# choc (header-only). Resolution order:
#   1. -DPULP_WCLAP_CHOC_INCLUDE=<dir containing choc/> (explicit override)
#   2. a populated sibling native build tree (build/_deps/choc-src)
#   3. FetchContent at the same pin as PulpDependencies.cmake (self-contained)
# The include dir must be the parent of `choc/`, since headers are <choc/...>.
if(NOT PULP_WCLAP_CHOC_INCLUDE)
    foreach(_cand
        "${_PULP_WCLAP_ROOT}/build/_deps/choc-src"
        "${_PULP_WCLAP_ROOT}/build-dbg/_deps/choc-src"
        "${_PULP_WCLAP_ROOT}/build-cov/_deps/choc-src")
        if(EXISTS "${_cand}/choc/audio/choc_MIDI.h")
            set(PULP_WCLAP_CHOC_INCLUDE "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(NOT PULP_WCLAP_CHOC_INCLUDE)
    # Mirrors PulpDependencies.cmake's choc pin (danielraffel/choc fork — the
    # webview-DnD PoC; the headers the WCLAP DSP subset needs are unaffected by
    # the fork, but the pin is matched for reproducibility). Keep in sync.
    include(FetchContent)
    FetchContent_Declare(
        choc_wclap
        GIT_REPOSITORY https://github.com/danielraffel/choc.git
        GIT_TAG pulp-webview-dnd-poc1)
    FetchContent_MakeAvailable(choc_wclap)
    # The choc checkout has a top-level choc/ subdir, so the checkout dir itself
    # is the include root (headers resolve as <choc/audio/...>).
    set(PULP_WCLAP_CHOC_INCLUDE "${choc_wclap_SOURCE_DIR}")
endif()

# CLAP headers (header-only, MIT). Fetched to match the native pin (1.2.9).
if(NOT PULP_WCLAP_CLAP_INCLUDE)
    include(FetchContent)
    FetchContent_Declare(
        clap_wclap
        GIT_REPOSITORY https://github.com/free-audio/clap.git
        GIT_TAG 1.2.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(clap_wclap)
    set(PULP_WCLAP_CLAP_INCLUDE "${clap_wclap_SOURCE_DIR}/include")
endif()

set(_PULP_WCLAP_INCLUDES
    ${_PULP_WCLAP_ROOT}/core/platform/include
    ${_PULP_WCLAP_ROOT}/core/runtime/include
    ${_PULP_WCLAP_ROOT}/core/timebase/include
    ${_PULP_WCLAP_ROOT}/core/timeline/include
    ${_PULP_WCLAP_ROOT}/core/playback/include
    ${_PULP_WCLAP_ROOT}/core/state/include
    ${_PULP_WCLAP_ROOT}/core/audio/include
    ${_PULP_WCLAP_ROOT}/core/midi/include
    ${_PULP_WCLAP_ROOT}/core/events/include
    ${_PULP_WCLAP_ROOT}/core/format/include
    ${_PULP_WCLAP_ROOT}/core/signal/include
    ${_PULP_WCLAP_WAV_INCLUDES}
    ${PULP_WCLAP_CHOC_INCLUDE}
    ${PULP_WCLAP_CLAP_INCLUDE}
)

# Portable DSP + CLAP-adapter subset — compiled once into an OBJECT library and
# shared across every WebCLAP plugin target.
set(_PULP_WCLAP_CORE_SOURCES
    ${_PULP_WCLAP_ROOT}/core/timebase/src/compiled_meter_map.cpp
    ${_PULP_WCLAP_ROOT}/core/timebase/src/compiled_tempo_map.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/assets.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/automation_curve.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/automation_document_internal.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/automation_lane.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/command.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/document_session.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/identity_directory.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/id_remap.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/journal.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/model.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/schema_codegen.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/schema_json.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/schema_json_canonical.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/schema_json_parser.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/schema_json_preflight.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/schema_json_summary.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/schema_registry.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/serialize_automation_decode.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/serialize_decode_support.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/serialize_decode.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/serialize_encode.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/snapshot_equivalence.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/structural_registry_validation.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/track.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/track_schema_migrations.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/take_lane_schema_migrations.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/take_lane.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/transaction.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/transaction_automation_internal.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/transaction_take_internal.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/transaction_track_state_internal.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/transaction_reduction_support.cpp
    ${_PULP_WCLAP_ROOT}/core/timeline/src/undo.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/audio_renderer.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/automation_cursor.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/automation_program.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/automation_program_compiler.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/compile_executor.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/note_renderer.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/program.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/program_compiler.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/stable_renderer_shell.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/track_automation_compiler.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/track_automation_program.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/track_automation_renderer.cpp
    ${_PULP_WCLAP_ROOT}/core/playback/src/transport.cpp
    ${_PULP_WCLAP_WAV_SOURCES}
    ${_PULP_WCLAP_ROOT}/core/format/src/clap_adapter.cpp
    ${_PULP_WCLAP_ROOT}/core/format/src/clap_remote_controls.cpp
    ${_PULP_WCLAP_ROOT}/core/format/src/processor_f64.cpp
    ${_PULP_WCLAP_ROOT}/core/format/src/host_quirks.cpp
    ${_PULP_WCLAP_ROOT}/core/format/src/host_version.cpp
    ${_PULP_WCLAP_ROOT}/core/format/src/host_type.cpp
    ${_PULP_WCLAP_ROOT}/core/format/src/registry.cpp
    ${_PULP_WCLAP_ROOT}/core/format/src/plugin_state_io.cpp
    ${_PULP_WCLAP_ROOT}/core/format/src/wasm/headless_defaults.cpp
    ${_PULP_WCLAP_ROOT}/core/events/src/plugin_main_thread_stub.cpp
    ${_PULP_WCLAP_ROOT}/core/events/src/wasm/main_thread_dispatcher_wasm.cpp
    ${_PULP_WCLAP_ROOT}/core/events/src/event_loop.cpp
    ${_PULP_WCLAP_ROOT}/core/state/src/store.cpp
    ${_PULP_WCLAP_ROOT}/core/state/src/state_migration.cpp
    ${_PULP_WCLAP_ROOT}/core/state/src/wasm/preset_manager_wasm.cpp
    ${_PULP_WCLAP_ROOT}/core/midi/src/mpe_voice_tracker.cpp
    ${_PULP_WCLAP_ROOT}/core/runtime/src/scoped_no_alloc.cpp
)

add_library(pulp-wclap-dsp OBJECT ${_PULP_WCLAP_CORE_SOURCES})
target_compile_features(pulp-wclap-dsp PUBLIC cxx_std_20)
target_include_directories(pulp-wclap-dsp PUBLIC ${_PULP_WCLAP_INCLUDES})
target_compile_definitions(pulp-wclap-dsp PUBLIC
    PULP_WCLAP=1 PULP_WASM=1 PULP_HEADLESS=1
    PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1)
target_compile_options(pulp-wclap-dsp PRIVATE -fno-exceptions -fno-rtti)

# pulp_add_wclap(<Name>
#     ENTRY    <entry.cpp>          # required: the WebCLAP entry point (PULP_WCLAP_PLUGIN)
#     [SOURCES <extra .cpp> ...]    # optional: extra plugin DSP sources
#     [INCLUDES <dir> ...])         # optional: extra include dirs (plugin headers)
#
# Emits <Name>.wasm exporting clap_entry + malloc/free/cabi_realloc + a growable
# function table + shared memory (the WebCLAP host contract). The reactor model,
# memory, and table link flags come from wasi-toolchain.cmake.
# Locate node once for the optional post-build bundling step.
find_program(_PULP_WCLAP_NODE node)

function(pulp_add_wclap NAME)
    cmake_parse_arguments(ARG "" "ENTRY;RESOURCES" "SOURCES;INCLUDES" ${ARGN})
    if(NOT ARG_ENTRY)
        message(FATAL_ERROR "pulp_add_wclap(${NAME}): ENTRY <entry.cpp> is required.")
    endif()

    add_executable(${NAME}-wclap
        $<TARGET_OBJECTS:pulp-wclap-dsp>
        ${ARG_ENTRY}
        ${ARG_SOURCES}
    )
    target_include_directories(${NAME}-wclap PRIVATE ${_PULP_WCLAP_INCLUDES} ${ARG_INCLUDES})
    # PULP_HEADLESS gates out every editor / core-view (canvas/Skia/text-shaping)
    # reference in a plugin header — none of that stack exists in a sandboxed
    # wasm CLAP, and it is not on the curated include set above. Plugin headers
    # guard their create_view()/editor bodies with `#if !PULP_HEADLESS` (same as
    # the WAM build); the base class's headless default then returns nullptr.
    # A pure-DSP plugin like PulpGain has no editor, so this is a harmless no-op
    # there. The shared DSP subset never branches on it (0 references).
    target_compile_definitions(${NAME}-wclap PRIVATE PULP_WCLAP=1 PULP_WASM=1 PULP_HEADLESS=1)

    # The reactor / memory / table flags are toolchain-level (see
    # wasi-toolchain.cmake), so only the clap_entry data export is per-target.
    set_target_properties(${NAME}-wclap PROPERTIES
        OUTPUT_NAME "${NAME}"
        SUFFIX ".wasm"
        LINK_FLAGS "-Wl,--export=clap_entry -Wl,--no-entry")

    # Emit the distributable WebCLAP bundle (<Name>.wclap/ with module.wasm +
    # resources, per free-audio/web-clap) as a post-build step. The bare .wasm
    # stays for the probe/host; the bundle is what a WebCLAP host or web
    # distribution consumes. Requires node (the packager is JS); skipped with a
    # status message otherwise so the wasm build never hard-fails on its absence.
    if(_PULP_WCLAP_NODE)
        set(_pack_args
            "${_PULP_WCLAP_NODE}"
            "${_PULP_WCLAP_ROOT}/core/format/src/wasm/pack-wclap.mjs"
            --wasm "$<TARGET_FILE:${NAME}-wclap>"
            --name "${NAME}"
            --out "${CMAKE_CURRENT_BINARY_DIR}"
            --tar)
        if(ARG_RESOURCES)
            list(APPEND _pack_args --resources "${ARG_RESOURCES}")
        endif()
        add_custom_command(TARGET ${NAME}-wclap POST_BUILD
            COMMAND ${_pack_args}
            COMMENT "Packaging ${NAME}.wclap bundle"
            VERBATIM)
    else()
        message(STATUS "PulpWclap: node not found — skipping ${NAME}.wclap bundle (bare .wasm only).")
    endif()
endfunction()
