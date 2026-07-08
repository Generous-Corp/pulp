# AssertNoTracingSymbols.cmake — BEST-EFFORT guard: a default (PULP_TRACING=OFF)
# build must contain ZERO Perfetto symbols.
#
# IMPORTANT: like AssertNoJsSymbols.cmake, this binary nm-scan is defence in
# depth, NOT the authoritative guarantee. The authoritative guarantee is that
# tools/cmake/PulpTracing.cmake adds no Perfetto link input when OFF (the
# pulp::tracing INTERFACE target is empty), so nothing to strip can exist. This
# scan still catches a Perfetto symbol force-referenced / whole-archived into a
# binary, and backstops the "never ship it enabled" contract.
#
# Invoked as a POST_BUILD step:
#   cmake -DBIN=<path-to-binary> -P tools/cmake/AssertNoTracingSymbols.cmake
if(NOT DEFINED BIN)
    message(FATAL_ERROR "AssertNoTracingSymbols: BIN not set")
endif()

find_program(NM_EXE nm)
if(NOT NM_EXE)
    message(WARNING "AssertNoTracingSymbols: 'nm' not found; skipping symbol check for ${BIN}")
    return()
endif()

execute_process(COMMAND "${NM_EXE}" "${BIN}" OUTPUT_VARIABLE _syms ERROR_QUIET)

# Perfetto's C++ SDK mangles its classes under the `perfetto` namespace, and the
# track-event macros emit `PERFETTO_`-prefixed storage. Either substring in an
# OFF build means the amalgamation leaked in.
set(_markers "perfetto" "PERFETTO_TRACK_EVENT")
set(_found "")
foreach(_m IN LISTS _markers)
    string(FIND "${_syms}" "${_m}" _idx)
    if(NOT _idx EQUAL -1)
        list(APPEND _found "${_m}")
    endif()
endforeach()

if(_found)
    message(FATAL_ERROR
        "PULP_TRACING=OFF build leaked Perfetto symbols into\n"
        "  ${BIN}\n"
        "  found: ${_found}\n"
        "Tracing is dev-only and must never be linked into a default/shipping "
        "build. Check that pulp::tracing stays an empty INTERFACE target when "
        "PULP_TRACING=OFF (tools/cmake/PulpTracing.cmake).")
endif()

message(STATUS "Tracing symbol check passed (no Perfetto symbols): ${BIN}")
