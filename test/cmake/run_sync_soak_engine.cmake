# Drives the engine-side sync soak in both directions.
#
# A soak that can only pass proves nothing, so this asserts the negative control
# too: a clean chase must be ACCEPTED and an over-budget drift must be REJECTED.
# If the verifier ever stopped discriminating, the second half fails here rather
# than the gate quietly going green forever.

file(MAKE_DIRECTORY "${WORKDIR}")

set(_clean "${WORKDIR}/soak-clean.json")
set(_drift "${WORKDIR}/soak-drift.json")

# ── 1. clean chase: no injected drift, must satisfy the spec ────────────────
execute_process(
    COMMAND "${CAPTURE}" --out "${_clean}" --duration 1800 --drift-ppm 0
            --captured-at 2026-07-27T00:00:00Z
    RESULT_VARIABLE _capture_result
    OUTPUT_VARIABLE _capture_out
    ERROR_VARIABLE _capture_err)
if(NOT _capture_result EQUAL 0)
    message(FATAL_ERROR
        "sync-soak capture (clean) failed: ${_capture_result}\n${_capture_out}${_capture_err}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "PULP_TIMELINE_SYNC_SOAK_SPEC=${SPEC}"
            "PULP_TIMELINE_SYNC_SOAK_TRACE=${_clean}"
            "${PYTHON}" "${VERIFIER}"
    RESULT_VARIABLE _clean_result
    OUTPUT_VARIABLE _clean_out
    ERROR_VARIABLE _clean_err)
if(NOT _clean_result EQUAL 0)
    message(FATAL_ERROR
        "A clean chase should satisfy the sync-soak spec but did not "
        "(exit ${_clean_result}). Either the chase regressed or the tolerances "
        "no longer describe correct behaviour.\n${_clean_out}${_clean_err}")
endif()

# ── 2. negative control: 40 ppm is over the 25 ppm ceiling, must be REJECTED ─
execute_process(
    COMMAND "${CAPTURE}" --out "${_drift}" --duration 1800 --drift-ppm 40
            --captured-at 2026-07-27T00:00:00Z
    RESULT_VARIABLE _drift_capture
    OUTPUT_VARIABLE _drift_cap_out
    ERROR_VARIABLE _drift_cap_err)
if(NOT _drift_capture EQUAL 0)
    message(FATAL_ERROR
        "sync-soak capture (drift) failed: ${_drift_capture}\n${_drift_cap_out}${_drift_cap_err}")
endif()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env
            "PULP_TIMELINE_SYNC_SOAK_SPEC=${SPEC}"
            "PULP_TIMELINE_SYNC_SOAK_TRACE=${_drift}"
            "${PYTHON}" "${VERIFIER}"
    RESULT_VARIABLE _drift_result
    OUTPUT_VARIABLE _drift_out
    ERROR_VARIABLE _drift_err)
if(_drift_result EQUAL 0)
    message(FATAL_ERROR
        "NEGATIVE CONTROL FAILED: a 40 ppm drift was accepted against a 25 ppm "
        "ceiling. The soak is no longer discriminating, so a passing clean run "
        "means nothing.\n${_drift_out}${_drift_err}")
endif()

message(STATUS "sync soak: clean chase accepted; 40 ppm drift rejected as expected")
