# AssertNoTracingSymbols.cmake — BEST-EFFORT guard: a default (PULP_TRACING=OFF)
# build must contain ZERO Perfetto symbols.
#
# IMPORTANT: like AssertNoJsSymbols.cmake, this binary nm-scan is defense in
# depth, NOT the authoritative guarantee. The authoritative guarantee is that
# tools/cmake/PulpTracing.cmake adds no Perfetto link input when OFF (the
# pulp::tracing INTERFACE target is empty), so nothing to strip can exist. This
# scan still catches a Perfetto symbol force-referenced / whole-archived into a
# binary, and backstops the "never ship it enabled" contract.
#
# A thin wrapper: the markers are the `tracing-off` gate of
# release_artifact_forbidden.json, applied by PulpReleaseArtifactScan.cmake.
#
# Invoked as a POST_BUILD step:
#   cmake -DBIN=<path-to-binary> -P tools/cmake/AssertNoTracingSymbols.cmake
if(NOT DEFINED BIN)
    message(FATAL_ERROR "AssertNoTracingSymbols: BIN not set")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/PulpReleaseArtifactScan.cmake")
pulp_release_artifact_load()
pulp_release_artifact_forbid(ARTIFACT "${BIN}" GATES tracing-off
    LABEL "PULP_TRACING=OFF symbol check")
