# Drives the engine-side sync soak in both directions.
#
# A soak that can only pass proves nothing, so this asserts a negative control
# too. Picking the right one matters:
#
# A DRIFTING REFERENCE IS NOT A FAILURE. The reference is the thing being
# chased, so wherever it has got to is correct by definition, and a chase that
# re-anchors on every lock agrees with it however far it drifts. Asserting that
# "40 ppm drift is rejected" would therefore be asserting the wrong thing — it
# would pass only while the harness modelled the reference's drift as Pulp's
# error.
#
# The real failure is a chase that stops re-anchoring. So the control feeds a
# reference that drifts AND a link that loses a quarter-frame from every cycle:
# with nothing to re-anchor to, the position carries forward at nominal rate and
# pulls away from the reference, which the offset ceiling catches.

file(MAKE_DIRECTORY "${WORKDIR}")

set(_clean "${WORKDIR}/soak-clean.json")
set(_drift "${WORKDIR}/soak-drift.json")

# ── 1. a correct chase tracks a DRIFTING reference and satisfies the spec ───
# Deliberately non-zero drift: at zero the chase and a free-run are
# indistinguishable, so a zero-drift-only control would pass even if
# re-anchoring were broken.
execute_process(
    COMMAND "${CAPTURE}" --out "${_clean}" --duration 1800 --drift-ppm 40
            --drop-one-in 0 --captured-at 2026-07-27T00:00:00Z
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
        "A chase re-anchoring against a 40 ppm reference should satisfy the "
        "sync-soak spec but did not (exit ${_clean_result}). Either re-anchoring "
        "regressed or the tolerances no longer describe correct behaviour."
        "\n${_clean_out}${_clean_err}")
endif()

# ── 2. negative control: a chase with nothing to re-anchor to must be REJECTED ─
execute_process(
    COMMAND "${CAPTURE}" --out "${_drift}" --duration 1800 --drift-ppm 40
            --drop-one-in 1 --captured-at 2026-07-27T00:00:00Z
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
        "NEGATIVE CONTROL FAILED: a chase that never re-anchors (every MTC cycle "
        "missing a quarter-frame, against a 40 ppm reference) was ACCEPTED. It "
        "should pull away from the reference and breach the offset ceiling, so "
        "the soak is no longer discriminating and a passing run above means "
        "nothing.\n${_drift_out}${_drift_err}")
endif()

message(STATUS
    "sync soak: chase tracked a 40 ppm reference; a never-re-anchoring chase was rejected")
