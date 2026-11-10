#!/usr/bin/env bash

# Run a pre-push gate with regular-file stdout/stderr so a caller's
# nonblocking output pipe cannot turn a healthy Python gate into EAGAIN.
_prepush_gate_seq=0
_prepush_gate_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_prepush_gate_supervisor="${_prepush_gate_lib_dir}/gate-supervisor.py"
# The gate that ran last, so a caller that decides it FAILED (prepush_fail) can
# name it. Kept separate from the gate's exit status: some gates exit non-zero
# without failing the push (a skipped lane), and only the caller knows.
_prepush_last_gate_label=""
_prepush_last_gate_log=""
_prepush_last_gate_status=0
_prepush_failed_gates=()
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
    _prepush_last_gate_label="$(_prepush_gate_label "$@")"
    _prepush_last_gate_log="$gate_log"
    _prepush_last_gate_status="$gate_status"
    return "$gate_status"
}

# The gate's own name: the first argument that names a script, so
# `env X=1 bash tools/scripts/foo.sh` and `python3 tools/scripts/bar.py` read as
# foo.sh and bar.py rather than env and python3.
_prepush_gate_label() {
    local arg
    for arg in "$@"; do
        case "$arg" in
            *.py|*.sh) echo "${arg##*/}"; return ;;
        esac
    done
    echo "${1##*/}"
}

# One line of a gate's output worth reading first: the first line that says
# what went wrong, else the last non-empty line. Only a pointer; the gate's own
# output above is the full account and carries its fix line.
_prepush_gate_excerpt() {
    local log="$1" why="" last="" fix=""
    [ -r "$log" ] || return 0
    why="$(grep -m1 -E '✗|FAILED|DRIFT|COULD NOT|BLOCKING|NOT updated|not [A-Za-z-]+ clean|grew past|without encoding|blocked|violat' "$log" 2>/dev/null || true)"
    last="$(grep -v '^[[:space:]]*$' "$log" 2>/dev/null | tail -n 1 || true)"
    # Gates end with their fix line (`Fix: ...`, `regenerate with: ...`); show it
    # next to the reason when it is a different line.
    case "$last" in
        *[Ff]ix:*|*egenerate*|*Add\ .github*|*[Rr]un\ *) fix="$last" ;;
    esac
    [ -n "$why" ] || why="$last"
    [ "$fix" != "$why" ] || fix=""
    why="${why#"${why%%[![:space:]]*}"}"
    fix="${fix#"${fix%%[![:space:]]*}"}"
    printf '%.110s' "$why"
    [ -z "$fix" ] || printf ' | %.80s' "$fix"
}

# Mark the push as failing and remember why. With a label the failing check is
# named by the caller (a check that is not a captured gate); without one it is
# the gate that ran last.
prepush_fail() {
    fail=1
    if [ -n "${1:-}" ]; then
        _prepush_failed_gates+=("$1|${2:-}")
    elif [ "$_prepush_last_gate_status" -ne 0 ] && [ -n "$_prepush_last_gate_label" ]; then
        _prepush_failed_gates+=("${_prepush_last_gate_label} (exit ${_prepush_last_gate_status})|$(_prepush_gate_excerpt "$_prepush_last_gate_log")")
        # Not reported twice if two arms fail on the same gate.
        _prepush_last_gate_status=0
    else
        _prepush_failed_gates+=("an uncaptured check|see the output above")
    fi
}

# The verdict line that was missing: which gates failed, with the line each
# printed that says why, so the cause is not 500 lines up among passing gates.
prepush_print_failed_gates() {
    [ "${#_prepush_failed_gates[@]}" -gt 0 ] || return 0
    local entry label excerpt
    echo "[pre-push] FAILED GATE(S):" >&2
    for entry in "${_prepush_failed_gates[@]}"; do
        label="${entry%%|*}"
        excerpt="${entry#*|}"
        echo "[pre-push]   - ${label}${excerpt:+: ${excerpt}}" >&2
    done
    echo "[pre-push]   Each gate's own output, with its fix line, is above; search for the name." >&2
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
