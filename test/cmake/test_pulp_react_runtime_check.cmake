cmake_minimum_required(VERSION 3.24)

# pulp_check_vendored_react_runtime(): a stale vendored @pulp/react bundle
# warns (or fails under STRICT) and names the missing fix; a current one is
# quiet. Each case runs in a child cmake so its warnings can be captured.

if(NOT PULP_SRC_DIR OR NOT FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SRC_DIR and FIXTURE_DIR are required")
endif()

file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}")

file(WRITE "${FIXTURE_DIR}/fingerprint.json" [=[
{"schema": 1, "revision": 2, "fixes": [
  {"revision": 1, "id": "scoped-reapply", "summary": "scoped re-apply",
   "signature": "materializedDirtyIds"},
  {"revision": 2, "id": "unsigned-fix", "summary": "a fix with no signature"}
]}
]=])

# An old banner-less bundle (the shape a pre-fingerprint import emitted).
file(WRITE "${FIXTURE_DIR}/stale.js"
    "(() => { function markMaterializedTreeDirty() {} })();\n")
# Banner-less, but carries the first fix's signature: only the unsigned fix
# can be judged missing.
file(WRITE "${FIXTURE_DIR}/partial.js"
    "(() => { const materializedDirtyIds = new Set(); })();\n")
# Stamped at the SDK's revision.
file(WRITE "${FIXTURE_DIR}/current.js"
    "/* @pulp/react runtime revision 2 */\n(() => {})();\n")

function(run_case name bundle extra out_rc out_log)
    set(_driver "${FIXTURE_DIR}/${name}.cmake")
    file(WRITE "${_driver}"
        "include(\"${PULP_SRC_DIR}/tools/cmake/PulpReactRuntime.cmake\")\n"
        "set(CMAKE_CURRENT_SOURCE_DIR \"${FIXTURE_DIR}\")\n"
        "pulp_check_vendored_react_runtime(\"${bundle}\" ${extra}"
        " FINGERPRINT \"${FIXTURE_DIR}/fingerprint.json\")\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_driver}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(${out_rc} "${_rc}" PARENT_SCOPE)
    set(${out_log} "${_out}${_err}" PARENT_SCOPE)
endfunction()

# `condition` is an if() expression written as one string; split it into the
# argument list if() expects.
function(expect condition what log)
    string(REPLACE " " ";" _condition "${condition}")
    if(NOT (${_condition}))
        message(FATAL_ERROR "${what}\n--- output ---\n${log}")
    endif()
endfunction()

run_case(stale stale.js "" _rc _log)
string(FIND "${_log}" "Vendored @pulp/react runtime is stale" _stale_at)
string(FIND "${_log}" "[scoped-reapply]" _fix1_at)
string(FIND "${_log}" "[unsigned-fix]" _fix2_at)
string(FIND "${_log}" "Refresh it by re-running" _refresh_at)
expect("_rc EQUAL 0" "a stale bundle must warn, not fail, by default" "${_log}")
expect("NOT _stale_at EQUAL -1" "a stale bundle was not reported stale" "${_log}")
expect("NOT _fix1_at EQUAL -1 AND NOT _fix2_at EQUAL -1"
    "the missing fixes were not both named" "${_log}")
expect("NOT _refresh_at EQUAL -1" "the refresh command was not given" "${_log}")

run_case(stale_strict stale.js STRICT _rc _log)
expect("NOT _rc EQUAL 0" "STRICT did not fail a stale bundle" "${_log}")

run_case(partial partial.js "" _rc _log)
string(FIND "${_log}" "[scoped-reapply]" _fix1_at)
string(FIND "${_log}" "[unsigned-fix]" _fix2_at)
expect("_fix1_at EQUAL -1" "a fix whose signature is present was reported missing"
    "${_log}")
expect("NOT _fix2_at EQUAL -1" "an unsigned fix past a banner-less bundle was not reported"
    "${_log}")

run_case(current current.js STRICT _rc _log)
string(FIND "${_log}" "is current" _current_at)
expect("_rc EQUAL 0 AND NOT _current_at EQUAL -1"
    "a bundle stamped at the SDK revision was not reported current" "${_log}")

# The checked-in fingerprint must parse and carry the signature the scoped
# re-apply fix compiles into every bundle built since.
file(READ "${PULP_SRC_DIR}/packages/pulp-react/runtime-fingerprint.json" _real)
string(JSON _real_revision GET "${_real}" revision)
string(JSON _real_signature GET "${_real}" fixes 0 signature)
expect("_real_revision GREATER_EQUAL 1" "the SDK fingerprint has no revision" "${_real}")
expect("_real_signature STREQUAL materializedDirtyIds"
    "the first fix's signature moved; update fixtures and bundles together" "${_real}")

message(STATUS "pulp_check_vendored_react_runtime: all cases passed")
