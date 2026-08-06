# Emscripten build of the timeline conformance runner.
#
# `core/interchange/tools/fixture_runner_main.cpp` states that the same binary
# runs under desktop ctest, on an Android emulator, and compiled to WASM. This
# root is what makes the third of those checkable: it produces
# `pulp-fixture-runner.js` + `.wasm`, which node runs against the real corpus
# with the real exit code, so the WASM claim is a lane that can go red rather
# than a sentence in a comment.
#
# Build + run:
#   emcmake cmake -S core/interchange/wasm -B build-wasm-fixture \
#       -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-wasm-fixture -j6
#   node build-wasm-fixture/pulp-fixture-runner.js \
#       --corpus test/fixtures/timeline
#
# WHY A SEPARATE ROOT INSTEAD OF `emcmake` ON THE REPO ROOT:
# the top-level project configures the desktop stack — Skia, Dawn, the format
# adapters, mbedTLS — none of which configures under Emscripten. The web lanes
# already solve this the same way (examples/web-demos/wasm-build,
# examples/web-demos/wclap-build): a small dedicated root that names exactly the
# portable sources it needs. This one names the interchange dependency floor.
#
# WHY A SOURCE LIST INSTEAD OF `pulp::timeline` / `pulp::interchange`:
# both targets link `pulp::runtime`, which links mbedTLS and an HTTP client.
# Neither belongs in, nor builds under, this toolchain. `PulpWam.cmake` carries
# the same note for the same reason.
#
# WHAT GUARDS THIS LIST, AND WHAT DOES NOT:
# `timeline_engine_dependency_floor_check.py` reads only `core/<module>/
# CMakeLists.txt` and the modules' own sources, so it does NOT see this file —
# naming an out-of-floor source here would not fail it. What the floor gate does
# cover is the runner's own includes, which is what decides what this list has
# to contain; every entry below is the transitive closure of those floored
# includes. The interchange entries additionally get the explicit drift check
# below, because that is the set most likely to grow.

cmake_minimum_required(VERSION 3.20)
project(pulp-fixture-runner-wasm LANGUAGES CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "This root is Emscripten-only. Configure it with "
        "`emcmake cmake -S core/interchange/wasm -B <dir>`. The desktop build "
        "of pulp-fixture-runner is declared by core/interchange/CMakeLists.txt.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PULP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

include(${PULP_ROOT}/core/timeline/PulpTimelineSources.cmake)
# The portable list only — `pulp_resolve_timeline_native_sources` is the
# filesystem-backed journal, which is exactly what must not enter a wasm build.
pulp_resolve_timeline_sources("${PULP_ROOT}" _PULP_FIXTURE_TIMELINE_SOURCES)

set(_PULP_FIXTURE_INTERCHANGE_SOURCES
    census.cpp
    export_plan.cpp
    loss.cpp)

# A new `core/interchange/src/*.cpp` that this root does not compile shows up as
# an undefined symbol only if the runner happens to reach it — otherwise the
# wasm lane keeps passing while quietly covering less of the module than the
# desktop build does. Compare against the directory and fail the configure
# instead, so adding a source is a decision made here rather than a silent
# omission.
file(GLOB _PULP_FIXTURE_INTERCHANGE_ON_DISK
    RELATIVE "${PULP_ROOT}/core/interchange/src"
    "${PULP_ROOT}/core/interchange/src/*.cpp")
list(SORT _PULP_FIXTURE_INTERCHANGE_ON_DISK)
set(_PULP_FIXTURE_INTERCHANGE_EXPECTED ${_PULP_FIXTURE_INTERCHANGE_SOURCES})
list(SORT _PULP_FIXTURE_INTERCHANGE_EXPECTED)
if(NOT _PULP_FIXTURE_INTERCHANGE_ON_DISK STREQUAL _PULP_FIXTURE_INTERCHANGE_EXPECTED)
    message(FATAL_ERROR
        "core/interchange/src has drifted from this root's source list.\n"
        "  on disk: ${_PULP_FIXTURE_INTERCHANGE_ON_DISK}\n"
        "  listed:  ${_PULP_FIXTURE_INTERCHANGE_EXPECTED}\n"
        "Add the source to _PULP_FIXTURE_INTERCHANGE_SOURCES if the wasm runner "
        "should compile it, or exclude it deliberately.")
endif()
list(TRANSFORM _PULP_FIXTURE_INTERCHANGE_SOURCES
    PREPEND "${PULP_ROOT}/core/interchange/src/")

add_executable(pulp-fixture-runner
    ${PULP_ROOT}/core/interchange/tools/fixture_runner_main.cpp
    ${_PULP_FIXTURE_INTERCHANGE_SOURCES}
    ${_PULP_FIXTURE_TIMELINE_SOURCES}
    ${PULP_ROOT}/core/timebase/src/compiled_meter_map.cpp
    ${PULP_ROOT}/core/timebase/src/compiled_tempo_map.cpp
    ${PULP_ROOT}/core/runtime/src/identity.cpp
    ${PULP_ROOT}/core/runtime/src/sha256.cpp
)

target_include_directories(pulp-fixture-runner PRIVATE
    ${PULP_ROOT}/core/platform/include
    ${PULP_ROOT}/core/runtime/include
    ${PULP_ROOT}/core/timebase/include
    ${PULP_ROOT}/core/music/include
    ${PULP_ROOT}/core/timeline/include
    ${PULP_ROOT}/core/interchange/include
)

# Matches how every other portable Pulp target is compiled.
target_compile_options(pulp-fixture-runner PRIVATE -fno-exceptions -fno-rtti -O2)

# NODERAWFS gives the wasm build the host filesystem, so `--corpus <path>` means
# the same thing here as it does natively and the corpus needs no preload or
# packaging step. EXIT_RUNTIME tears the runtime down when `main` returns, which
# is what a run-once CLI wants; the exit code reaches the shell either way under
# emscripten 6 (measured — do not treat this flag as what makes the lane able to
# go red; the lane asserts its own red in tools/ci/wasm-fixture-lane.sh).
target_link_options(pulp-fixture-runner PRIVATE
    -sNODERAWFS=1
    -sEXIT_RUNTIME=1
    -sALLOW_MEMORY_GROWTH=1
)
