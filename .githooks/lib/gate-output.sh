#!/usr/bin/env bash

# Run a pre-push gate with regular-file stdout/stderr so a caller's
# nonblocking output pipe cannot turn a healthy Python gate into EAGAIN.
_prepush_gate_seq=0
_prepush_gate_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_prepush_gate_supervisor="${_prepush_gate_lib_dir}/gate-supervisor.py"
run_gate_captured() {
    if [ -z "${PREPUSH_GATE_LOG_DIR:-}" ] || [ ! -d "$PREPUSH_GATE_LOG_DIR" ]; then
        echo "[pre-push] gate capture directory is unavailable" >&2
        return 2
    fi

    _prepush_gate_seq=$((_prepush_gate_seq + 1))
    local gate_log="$PREPUSH_GATE_LOG_DIR/gate-${_prepush_gate_seq}.log"
    # The supervisor owns the old `"$@" >"$gate_log" 2>&1` regular-file
    # capture contract while also containing every descendant in its own
    # cancellable process group.
    local supervisor_args=("${PYTHON:-python3}" "$_prepush_gate_supervisor" --log "$gate_log" -- "$@")
    "${supervisor_args[@]}"
    local gate_status=$?
    cat "$gate_log" >&2 || true
    return "$gate_status"
}

# A gate that could not RUN is not a gate that passed.
#
# Exit-code contract shared by Pulp's gate scripts:
#   0   the gate ran and the check PASSED
#   1   the gate ran and the check FAILED
#   2+  the gate could NOT run — missing/unreadable config, an empty corpus,
#       or a crash. It checked nothing.
#
# 2+ used to print "internal error" and fall through as a pass, so a gate that
# could not find its config reported SUCCESS. That false green let a missing
# version bump ride to main: no tag, no release. "I could not measure" is not
# "it is fine" — an unmeasurable gate blocks, and an operator demotes it
# deliberately with PULP_DISABLE_PREPUSH_GATES=1 rather than by accident.
gate_could_not_run() {
    local name="$1" status="$2"
    echo "[pre-push] $name: COULD NOT RUN (exit $status) — blocking the push." >&2
    echo "[pre-push]   This gate performed no check, so it cannot report a pass." >&2
    echo "[pre-push]   Usually a missing or unreadable config; the gate's own output" >&2
    echo "[pre-push]   above names the file it looked for and the path it looked at." >&2
    echo "[pre-push]   If it is genuinely unrelated to your change, demote deliberately:" >&2
    echo "[pre-push]     PULP_DISABLE_PREPUSH_GATES=1 git push" >&2
}
