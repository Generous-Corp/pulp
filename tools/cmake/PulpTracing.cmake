# PulpTracing.cmake — Perfetto tracing (DEV ONLY, OFF by default).
#
# Off-by-default developer tooling. When PULP_TRACING=OFF — the default and the
# ONLY supported shipping configuration — this module defines an INTERFACE target
# (pulp::tracing) that pulls in ZERO Perfetto headers, symbols, or link inputs,
# and the macros in <pulp/runtime/trace.hpp> compile to nothing. When ON, it
# fetches the version-pinned Perfetto amalgamation (Apache-2.0), compiles it as a
# PIC static lib, links it into the interface target, and sets
# PULP_TRACING_ENABLED=1 so the trace macros light up.
#
# NEVER ship a binary built with PULP_TRACING=ON. Two guards enforce this:
#   - test/test_tracing.cpp — Catch2 "off by default" assertion (a default build
#     is OFF, so it passes in normal CI and fails loudly if ON leaks in).
#   - tools/cmake/AssertNoTracingSymbols.cmake — best-effort nm scan (mirrors
#     AssertNoJsSymbols.cmake); the authoritative guarantee is this module adding
#     no Perfetto link input when OFF.
#
# D1 (planning/2026-07-08-perfetto-tracing-plan.md §0c) proved Perfetto's
# TRACE_EVENT is NOT real-time-safe (it locks a mutex on chunk rollover), so this
# subsystem instruments UI / render / process-level and OFFLINE-audio only —
# never the live audio thread's process() path.

include_guard(GLOBAL)

option(PULP_TRACING "Perfetto tracing (dev only; OFF by default; never ship enabled)" OFF)

add_library(pulp-tracing INTERFACE)
add_library(pulp::tracing ALIAS pulp-tracing)

if(NOT PULP_TRACING)
    # OFF: no Perfetto anything. <pulp/runtime/trace.hpp> is a header-only no-op.
    return()
endif()

message(STATUS
    "Pulp: PULP_TRACING=ON — DEV ONLY Perfetto tracing is compiled in. "
    "Do NOT ship this binary (see PulpTracing.cmake).")

# The Perfetto amalgamation ships as a GitHub release ARTIFACT
# (perfetto-cpp-sdk-src.zip); it is no longer in the git tree at sdk/. Pin by
# URL + SHA-256 (URL_HASH), the same trust posture Pulp uses for Skia
# (tools/deps/manifest.json). Verified against v57.2.
set(PULP_PERFETTO_VERSION "v57.2" CACHE STRING "Pinned Perfetto SDK release tag")
set(PULP_PERFETTO_SHA256
    "c6fa3d89aee30f7da39402c9cd178c9f2e344544fda5c2109fd8457e319c3a2f")

include(FetchContent)
FetchContent_Declare(perfetto
    URL "https://github.com/google/perfetto/releases/download/${PULP_PERFETTO_VERSION}/perfetto-cpp-sdk-src.zip"
    URL_HASH "SHA256=${PULP_PERFETTO_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# The archive is just perfetto.h + perfetto.cc (no CMakeLists), so MakeAvailable
# only populates — it does not add_subdirectory.
FetchContent_MakeAvailable(perfetto)

add_library(pulp-perfetto STATIC "${perfetto_SOURCE_DIR}/perfetto.cc")
target_include_directories(pulp-perfetto PUBLIC "${perfetto_SOURCE_DIR}")
set_target_properties(pulp-perfetto PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Silence the amalgamation's own warnings so it never trips a -Werror tree; the
# MSVC flags are the hard-won set from melatonin_perfetto (large TU + conformance).
target_compile_options(pulp-perfetto PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-w>
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj;/Zc:__cplusplus;/Zc:preprocessor;/permissive->)

target_link_libraries(pulp-tracing INTERFACE pulp-perfetto)
target_compile_definitions(pulp-tracing INTERFACE PULP_TRACING_ENABLED=1)
