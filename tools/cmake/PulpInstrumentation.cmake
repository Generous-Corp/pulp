# Pulp coverage instrumentation (#290 / coverage-sanitizers spec).
#
# Sanitizers are handled by the pre-existing Sanitizers.cmake
# (PULP_SANITIZER=address/thread/undefined/memory/realtime). This
# module adds the third leg of the hardening pipeline: Clang
# source-based coverage. Together they give:
#   - coverage (non-blocking, artifact only)
#   - sanitizers (blocking in CI via PULP_SANITIZER=address)
#
# Usage:
#   cmake -S . -B build-coverage -DPULP_ENABLE_COVERAGE=ON
#   cmake --build build-coverage
#   ctest --test-dir build-coverage
#   # Then: scripts/run_coverage.sh (wraps the above + llvm-cov report)
#
# Flags applied centrally so every Pulp target gets the same treatment
# without per-subsystem drift. External FetchContent deps (Skia, Dawn,
# mbedTLS, QuickJS, etc.) are compiled separately and not affected.
#
# Full design + phased rollout:
# planning/coverage-sanitizers-spec-2026-04-16.md (private submodule).

include_guard(GLOBAL)

option(PULP_ENABLE_COVERAGE "Clang source-based coverage instrumentation" OFF)

if(NOT PULP_ENABLE_COVERAGE)
    return()
endif()

# Coverage requires Clang for BOTH C and C++ compilers. If CC is
# gcc while CXX is clang++, the coverage flags produce incompatible
# profraw shapes and llvm-cov can't merge them. #317 Codex P2.
if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    message(FATAL_ERROR
        "PULP_ENABLE_COVERAGE requires Clang for C++ "
        "(CMAKE_CXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}). "
        "GCC gcov is not supported in year 1 — shape differences "
        "between llvm-cov and gcovr would confuse the per-subsystem "
        "summary table.")
endif()
if(NOT (CMAKE_C_COMPILER_ID MATCHES "Clang"))
    message(FATAL_ERROR
        "PULP_ENABLE_COVERAGE requires Clang for C "
        "(CMAKE_C_COMPILER_ID=${CMAKE_C_COMPILER_ID}). "
        "Mixing gcc for .c and clang++ for .cpp produces profraw "
        "shape incompatibilities that llvm-cov can't merge.")
endif()

# Warn on Release.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    message(WARNING
        "PULP_ENABLE_COVERAGE is ON with a Release build. Coverage "
        "instrumentation is most useful with Debug or RelWithDebInfo.")
endif()

# Disallow combining with sanitizers — shared link-time flags collide
# on some libc++/libstdc++ versions and we want separate artifacts for
# separate analyses.
if(PULP_SANITIZER)
    message(FATAL_ERROR
        "PULP_ENABLE_COVERAGE is mutually exclusive with PULP_SANITIZER. "
        "Pick one; use separate build directories.")
endif()

set(_pulp_coverage_compile_flags
    -fprofile-instr-generate
    -fcoverage-mapping
    -g
    -O0)
set(_pulp_coverage_link_flags
    -fprofile-instr-generate
    -fcoverage-mapping)

# Global application: coverage is all-or-nothing across the Pulp
# tree (it's a build-config thing, not per-target). Using
# add_compile_options + add_link_options keeps it consistent with
# Sanitizers.cmake and means no per-subsystem wiring change.
add_compile_options(${_pulp_coverage_compile_flags})
add_link_options(${_pulp_coverage_link_flags})

# Expose a helper variable so scripts/CI can confirm the build is
# instrumented (read back by llvm-cov report as a sanity check).
set(PULP_COVERAGE_ENABLED TRUE CACHE INTERNAL
    "Source-based coverage is active for this build tree")

message(STATUS "Pulp coverage: Clang source-based instrumentation ON")
