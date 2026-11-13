# Runs the real `pulp graph validate` on a G-11 fixture notation file and
# requires the documented refusal: exit 1, code UnsupportedContract, the named
# node, and the unregistered type in the rule. If the validator ever accepts
# the file, or fails for a different reason, this fails.
foreach(_v PULP_CLI PAT NODE TYPE)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "g11_notation_refusal: -D${_v} is required")
    endif()
endforeach()
execute_process(COMMAND "${PULP_CLI}" graph validate "${PAT}" --json
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 1)
    message(FATAL_ERROR "expected refusal exit 1, got ${_rc}\n${_out}${_err}")
endif()
string(JSON _valid GET "${_out}" valid)
string(JSON _code GET "${_out}" code)
string(JSON _node GET "${_out}" node)
string(JSON _rule GET "${_out}" rule)
if(_valid OR NOT _code STREQUAL "UnsupportedContract" OR NOT _node STREQUAL "${NODE}"
   OR NOT _rule STREQUAL "unregistered type/version '${TYPE}'")
    message(FATAL_ERROR "unexpected validator output for ${PAT}:\n${_out}")
endif()
message(STATUS "refused as documented: ${_rule} (node ${_node})")
