# Negative control for PulpReleaseArtifactScan.cmake. FORBIDDEN_BIN is built
# from the generated fixture that carries every symbol and string marker of
# tools/cmake/release_artifact_forbidden.json; the scanner must flag each one,
# through the module function and through the `cmake -P` entry point the ctest
# wrappers use. Dependency markers are loader-table entries no translation unit
# can fake, so each is fed to the probe as a synthetic otool/readelf line and
# must be flagged there. CLEAN_BIN carries nothing and must pass every gate.
cmake_minimum_required(VERSION 3.24)
foreach(_required PULP_SOURCE_DIR FORBIDDEN_BIN CLEAN_BIN)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()
set(_scanner "${PULP_SOURCE_DIR}/tools/cmake/PulpReleaseArtifactScan.cmake")
include("${_scanner}")
pulp_release_artifact_load()
set(_all_gates ${PULP_RELEASE_ARTIFACT_GATE_IDS})

# 1. Module path: every symbol and string marker present in the forbidden fixture.
pulp_release_artifact_select(OUT_IDS _expected_sym OUT_PATTERNS _unused KIND symbol GATES ${_all_gates})
pulp_release_artifact_select(OUT_IDS _expected_str OUT_PATTERNS _unused KIND string GATES ${_all_gates})
pulp_release_artifact_select(OUT_IDS _expected_dep OUT_PATTERNS _dep_patterns KIND dependency GATES ${_all_gates})
list(LENGTH _expected_sym _n_sym)
list(LENGTH _expected_str _n_str)
list(LENGTH _expected_dep _n_dep)
if(_n_sym EQUAL 0 OR _n_str EQUAL 0 OR _n_dep EQUAL 0)
    message(FATAL_ERROR
        "negative control needs at least one marker of each kind (symbol=${_n_sym} string=${_n_str} dependency=${_n_dep})")
endif()
pulp_release_artifact_probe(ARTIFACT "${FORBIDDEN_BIN}" GATES ${_all_gates}
    OUT_PRESENT _present OUT_ABSENT _absent OUT_UNCHECKED _unchecked)
foreach(_record IN LISTS _absent)
    if(NOT _record MATCHES "\\(dependency\\)")
        message(FATAL_ERROR
            "negative control: the forbidden fixture carries this marker but the scanner did not see it: ${_record}")
    endif()
endforeach()
list(LENGTH _present _n_present)
math(EXPR _n_expected "${_n_sym} + ${_n_str}")
if(NOT _n_present EQUAL _n_expected)
    message(FATAL_ERROR
        "negative control: expected ${_n_expected} symbol+string hits in the forbidden fixture, got ${_n_present}: ${_present}")
endif()
# The loader table was not supplied, so dependency markers must be reported
# unchecked rather than silently absent.
list(LENGTH _unchecked _n_unchecked)
if(NOT _n_unchecked EQUAL _n_dep)
    message(FATAL_ERROR
        "negative control: ${_n_dep} dependency markers should be UNCHECKED without a loader table, got ${_n_unchecked}: ${_unchecked}")
endif()

# 2. Dependency markers against a synthetic loader table, one entry at a time,
# in the three formats the control-shipping scanner reads.
foreach(_pattern IN LISTS _dep_patterns)
    foreach(_line
            "\t@rpath/lib${_pattern}.dylib (compatibility version 1.0.0, current version 1.0.0)"
            " 0x0000000000000001 (NEEDED)             Shared library: [lib${_pattern}.so]"
            "    ${_pattern}.dll")
        set(_table "${_line}")
        pulp_release_artifact_probe(ARTIFACT "${CLEAN_BIN}" GATES ${_all_gates}
            DEPENDENCIES _table OUT_PRESENT _dep_present OUT_ABSENT _dep_absent OUT_UNCHECKED _dep_unchecked)
        set(_hit FALSE)
        foreach(_record IN LISTS _dep_present)
            if(_record MATCHES "\\(dependency\\) ${_pattern}$")
                set(_hit TRUE)
            endif()
        endforeach()
        if(NOT _hit)
            message(FATAL_ERROR
                "negative control: dependency marker '${_pattern}' not flagged in loader line '${_line}'")
        endif()
    endforeach()
endforeach()

# 3. Clean control: nothing flagged for any gate, with an empty loader table so
# dependency markers are checked too.
set(_empty_table "")
pulp_release_artifact_probe(ARTIFACT "${CLEAN_BIN}" GATES ${_all_gates}
    DEPENDENCIES _empty_table OUT_PRESENT _clean_present OUT_ABSENT _clean_absent OUT_UNCHECKED _clean_unchecked)
if(_clean_present)
    message(FATAL_ERROR "negative control: the clean fixture was flagged: ${_clean_present}")
endif()
if(_clean_unchecked)
    message(FATAL_ERROR "negative control: markers left unchecked on the clean fixture: ${_clean_unchecked}")
endif()

# 4. Script entry point: the `cmake -P` form the ctest wrappers run must exit
# non-zero on the forbidden fixture and name every marker id, and exit zero on
# the clean one.
string(REPLACE ";" "\;" _gates_arg "${_all_gates}")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DBIN=${FORBIDDEN_BIN}" "-DGATES=${_all_gates}"
    -P "${_scanner}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0)
    message(FATAL_ERROR "negative control: script-mode scan passed the forbidden fixture")
endif()
foreach(_id IN LISTS _expected_sym _expected_str)
    string(FIND "${_out}${_err}" "${_id} (" _idx)
    if(_idx EQUAL -1)
        message(FATAL_ERROR "negative control: script-mode output does not name marker '${_id}':\n${_out}${_err}")
    endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DBIN=${CLEAN_BIN}" "-DGATES=${_all_gates}"
    -P "${_scanner}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "negative control: script-mode scan rejected the clean fixture:\n${_out}${_err}")
endif()
# The positive-control mode must likewise pass the forbidden fixture and
# reject the clean one.
execute_process(COMMAND "${CMAKE_COMMAND}" "-DBIN=${CLEAN_BIN}" "-DGATES=${_all_gates}" -DMODE=require
    -P "${_scanner}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0)
    message(FATAL_ERROR "negative control: require mode accepted the clean fixture")
endif()

message(STATUS
    "release-artifact negative control OK: ${_n_sym} symbol + ${_n_str} string markers flagged in the "
    "forbidden fixture, ${_n_dep} dependency markers flagged in synthetic loader tables, clean fixture passes")
