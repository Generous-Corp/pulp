# Self-test of tools/cmake/release_artifact_forbidden.json: the list loads,
# is non-empty, every gate is documented and used, and every marker is
# well-formed (known kind, searchable pattern, declared gates, a reason) with
# a unique id and a unique (kind, pattern). A scanner over a malformed or
# empty list is the vacuous pass this list exists to prevent.
cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR is required")
endif()
include("${PULP_SOURCE_DIR}/tools/cmake/PulpReleaseArtifactScan.cmake")
pulp_release_artifact_load()

if(NOT PULP_RELEASE_ARTIFACT_MARKER_COUNT GREATER 0)
    message(FATAL_ERROR "forbidden-marker list is empty: ${PULP_RELEASE_ARTIFACT_FORBIDDEN_LIST}")
endif()
list(LENGTH PULP_RELEASE_ARTIFACT_GATE_IDS _gate_count)
if(NOT _gate_count GREATER 0)
    message(FATAL_ERROR "forbidden-marker list declares no gate")
endif()

file(READ "${PULP_RELEASE_ARTIFACT_FORBIDDEN_LIST}" _json)
string(JSON _gates GET "${_json}" gates)
foreach(_gate IN LISTS PULP_RELEASE_ARTIFACT_GATE_IDS)
    if(NOT _gate MATCHES "^[a-z][a-z0-9-]*$")
        message(FATAL_ERROR "gate id '${_gate}' is not lowercase kebab")
    endif()
    string(JSON _doc GET "${_gates}" "${_gate}")
    string(LENGTH "${_doc}" _doc_len)
    if(_doc_len LESS 20)
        message(FATAL_ERROR "gate '${_gate}' needs a description of the build condition it names")
    endif()
endforeach()

set(_ids "")
set(_kind_patterns "")
set(_used_gates "")
math(EXPR _last "${PULP_RELEASE_ARTIFACT_MARKER_COUNT} - 1")
foreach(_i RANGE ${_last})
    set(_id "${PULP_RELEASE_ARTIFACT_MARKER_${_i}_ID}")
    set(_kind "${PULP_RELEASE_ARTIFACT_MARKER_${_i}_KIND}")
    set(_pattern "${PULP_RELEASE_ARTIFACT_MARKER_${_i}_PATTERN}")
    pulp_release_artifact_validate_marker("${_id}" "${_kind}" "${_pattern}"
        "${PULP_RELEASE_ARTIFACT_MARKER_${_i}_GATES}"
        "${PULP_RELEASE_ARTIFACT_MARKER_${_i}_REASON}" _problem)
    if(_problem)
        message(FATAL_ERROR "forbidden-marker list entry ${_i}: ${_problem}")
    endif()
    if(_id IN_LIST _ids)
        message(FATAL_ERROR "duplicate marker id '${_id}'")
    endif()
    list(APPEND _ids "${_id}")
    if("${_kind}|${_pattern}" IN_LIST _kind_patterns)
        message(FATAL_ERROR "duplicate ${_kind} pattern '${_pattern}' (marker '${_id}')")
    endif()
    list(APPEND _kind_patterns "${_kind}|${_pattern}")
    list(APPEND _used_gates ${PULP_RELEASE_ARTIFACT_MARKER_${_i}_GATES})
endforeach()
foreach(_gate IN LISTS PULP_RELEASE_ARTIFACT_GATE_IDS)
    if(NOT _gate IN_LIST _used_gates)
        message(FATAL_ERROR "gate '${_gate}' is declared but no marker uses it")
    endif()
    # A stripped binary has no symbol table, so a gate with only symbol
    # markers would be unchecked there; every gate needs a string backstop.
    pulp_release_artifact_select(OUT_IDS _gate_strings OUT_PATTERNS _gate_string_patterns
        KIND string GATES ${_gate})
    if(NOT _gate_strings)
        message(FATAL_ERROR "gate '${_gate}' has no string marker; a stripped binary would pass it unchecked")
    endif()
endforeach()

# check_control_shipping_artifact.cmake extracts printable runs of at least 14
# bytes (its _pulp_marker_min_length) and refuses a shorter marker at scan
# time; hold the list to the same floor so a short entry fails here, not on
# the first control-shipping artifact that links.
pulp_release_artifact_select(OUT_IDS _control_ids OUT_PATTERNS _control_patterns
    KIND string GATES control-stripped)
foreach(_pattern IN LISTS _control_patterns)
    string(LENGTH "${_pattern}" _len)
    if(_len LESS 14)
        message(FATAL_ERROR
            "control-stripped string marker '${_pattern}' is ${_len} bytes; the control-shipping scanner's run floor is 14")
    endif()
endforeach()

# Every pattern must be expressible by the negative-control fixture generator,
# otherwise a marker could exist that the negative control never proves.
pulp_release_artifact_write_fixture("${CMAKE_CURRENT_BINARY_DIR}/release_artifact_list_selftest_fixture.cpp")

message(STATUS
    "release-artifact forbidden list OK: ${PULP_RELEASE_ARTIFACT_MARKER_COUNT} markers across "
    "${_gate_count} gates (${PULP_RELEASE_ARTIFACT_GATE_IDS})")
