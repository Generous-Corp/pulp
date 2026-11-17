# Runs the patch printed in docs/guides/graph-first-steps.md through the real
# `pulp graph validate` and requires every `format_version: N` the two graph
# guides print for it to equal what the CLI prints. With -DSTALE_CONTROL=ON the
# same comparison is run against guide text whose number is moved off the
# CLI's, and the check must refuse it; a comparison that cannot fail would
# pass that control.
foreach(_v PULP_CLI GUIDE COOKBOOK WORK_DIR)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "graph_guide_format_version: -D${_v} is required")
    endif()
endforeach()

file(READ "${GUIDE}" _guide)
file(READ "${COOKBOOK}" _cookbook)

# The first fenced text block that opens with the notation header is the patch.
string(REGEX MATCH "```text\n(# flow 1\n[^`]*)```" _unused "${_guide}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "no `# flow 1` patch block found in ${GUIDE}")
endif()
set(_patch "${CMAKE_MATCH_1}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/first.pat" "${_patch}")

execute_process(COMMAND "${PULP_CLI}" graph validate "${WORK_DIR}/first.pat"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "guide patch did not validate (exit ${_rc})\n${_out}${_err}")
endif()
string(REGEX MATCH "format_version: ([0-9]+)" _unused "${_out}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "validate printed no format_version:\n${_out}")
endif()
set(_actual "${CMAKE_MATCH_1}")

if(STALE_CONTROL)
    math(EXPR _stale "${_actual} - 1")
    string(REGEX REPLACE "format_version: [0-9]+" "format_version: ${_stale}" _guide "${_guide}")
    string(REGEX REPLACE "format_version: [0-9]+" "format_version: ${_stale}" _cookbook "${_cookbook}")
endif()

set(_stated "")
foreach(_text "${_guide}" "${_cookbook}")
    string(REGEX MATCHALL "format_version: [0-9]+" _hits "${_text}")
    list(APPEND _stated ${_hits})
endforeach()
list(LENGTH _stated _count)
if(_count LESS 2)
    message(FATAL_ERROR "expected a format_version statement in each guide, found ${_count}")
endif()

set(_stale_found FALSE)
foreach(_hit IN LISTS _stated)
    if(NOT _hit STREQUAL "format_version: ${_actual}")
        set(_stale_found TRUE)
        message(STATUS "stale statement '${_hit}' (CLI prints ${_actual})")
    endif()
endforeach()

if(STALE_CONTROL)
    if(NOT _stale_found)
        message(FATAL_ERROR "control: the stale guide text was not detected")
    endif()
    message(STATUS "control: stale text detected as required")
elseif(_stale_found)
    message(FATAL_ERROR "guide format_version does not match the CLI (${_actual})")
else()
    message(STATUS "${_count} guide statements match the CLI format_version ${_actual}")
endif()
