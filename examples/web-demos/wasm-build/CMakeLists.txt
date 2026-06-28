# WASM build for Pulp web demo plugins
# Compiles PulpGain, PulpPluck, PulpChorus to WebAssembly via Emscripten
#
# Build:
#   cd examples/web-demos/wasm-build
#   emcmake cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Output: build/*.js + build/*.wasm files for the browser host

cmake_minimum_required(VERSION 3.20)
project(pulp-wasm-demos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

# Root of the Pulp repo
set(PULP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# Include paths for Pulp headers
set(PULP_INCLUDES
    ${PULP_ROOT}/core/platform/include
    ${PULP_ROOT}/core/runtime/include
    ${PULP_ROOT}/core/state/include
    ${PULP_ROOT}/core/audio/include
    ${PULP_ROOT}/core/midi/include
    ${PULP_ROOT}/core/events/include
    ${PULP_ROOT}/core/format/include
    ${PULP_ROOT}/core/signal/include
    ${PULP_ROOT}/examples/web-demos
    ${PULP_ROOT}/examples/pulp-gain
    ${PULP_ROOT}/examples/pulp-pluck
)

# CHOC headers (for MIDI)
if(EXISTS "${PULP_ROOT}/build/_deps/choc-src")
    list(APPEND PULP_INCLUDES "${PULP_ROOT}/build/_deps/choc-src")
elseif(EXISTS "${PULP_ROOT}/../pulp/build/_deps/choc-src")
    list(APPEND PULP_INCLUDES "${PULP_ROOT}/../pulp/build/_deps/choc-src")
endif()

# Emscripten compile flags
add_compile_options(
    -fno-exceptions
    -fno-rtti
    -O2
)

# Common Emscripten link flags for all WASM targets
set(WASM_LINK_FLAGS
    "-sALLOW_MEMORY_GROWTH=1"
    "-sENVIRONMENT=web"
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    # The wam_* C exports MUST be on this allowlist. With MODULARIZE+EXPORT_ES6
    # the generated module only emits Module['_name'] accessors for names listed
    # here; __attribute__((used)) keeps them in the binary but does NOT add the
    # JS accessor. Keep this in sync with the wam_* entry points
    # (pulp_*_wasm.cpp). Leading underscore is the C symbol prefix.
    "-sEXPORTED_FUNCTIONS=['_malloc','_free','_wam_init','_wam_process','_wam_set_param','_wam_get_param','_wam_midi','_wam_descriptor','_wam_state_size','_wam_read_state','_wam_write_state']"
    # HEAPF32/HEAPU8 are needed to marshal audio buffers and state blobs across
    # the JS/WASM boundary by copying into heap views.
    "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','HEAPF32','HEAPU8']"
    "--no-entry"
    "-O2"
)

string(JOIN " " WASM_LINK_FLAGS_STR ${WASM_LINK_FLAGS})

# Pulp source files needed for WASM (minimal set — no platform-specific code).
# Keep this list in sync with the transitive deps of the WAM bridge: store.cpp
# pulls EventLoop (event_loop.cpp) and state migration (state_migration.cpp);
# the Processor base's create_view()/create_ara_document_controller() defaults
# live in format.cpp.
set(PULP_CORE_SOURCES
    ${PULP_ROOT}/core/runtime/src/runtime.cpp
    ${PULP_ROOT}/core/runtime/src/identity.cpp
    ${PULP_ROOT}/core/state/src/store.cpp
    ${PULP_ROOT}/core/state/src/state_migration.cpp
    ${PULP_ROOT}/core/events/src/event_loop.cpp
    ${PULP_ROOT}/core/format/src/registry.cpp
    # Headless replacements for Processor::create_view() (format.cpp pulls
    # core/view -> canvas) and create_ara_document_controller() (ara.cpp pulls
    # the optional ARA SDK). The WASM DSP module is headless and never calls
    # either; core/format owns these WASM-only defaults (see the file header).
    ${PULP_ROOT}/core/format/src/wasm/headless_defaults.cpp
)

# ── PulpGain WASM ────────────────────────────────────────────────────────

add_executable(pulp-gain-wasm
    ${PULP_CORE_SOURCES}
    ${PULP_ROOT}/core/format/src/wasm/wam_adapter.cpp
    pulp_gain_wasm.cpp
)
target_include_directories(pulp-gain-wasm PRIVATE ${PULP_INCLUDES})
set_target_properties(pulp-gain-wasm PROPERTIES
    OUTPUT_NAME "PulpGain"
    SUFFIX ".js"
    LINK_FLAGS "${WASM_LINK_FLAGS_STR}"
)

# ── PulpPluck WASM ───────────────────────────────────────────────────────

add_executable(pulp-pluck-wasm
    ${PULP_CORE_SOURCES}
    ${PULP_ROOT}/core/format/src/wasm/wam_adapter.cpp
    pulp_pluck_wasm.cpp
)
target_include_directories(pulp-pluck-wasm PRIVATE ${PULP_INCLUDES})
set_target_properties(pulp-pluck-wasm PROPERTIES
    OUTPUT_NAME "PulpPluck"
    SUFFIX ".js"
    LINK_FLAGS "${WASM_LINK_FLAGS_STR}"
)

# ── PulpChorus WASM ──────────────────────────────────────────────────────

add_executable(pulp-chorus-wasm
    ${PULP_CORE_SOURCES}
    ${PULP_ROOT}/core/format/src/wasm/wam_adapter.cpp
    pulp_chorus_wasm.cpp
)
target_include_directories(pulp-chorus-wasm PRIVATE ${PULP_INCLUDES})
set_target_properties(pulp-chorus-wasm PROPERTIES
    OUTPUT_NAME "PulpChorus"
    SUFFIX ".js"
    LINK_FLAGS "${WASM_LINK_FLAGS_STR}"
)
