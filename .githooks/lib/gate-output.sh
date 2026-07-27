#!/usr/bin/env bash

# Run a pre-push gate with regular-file stdout/stderr so a caller's
# nonblocking output pipe cannot turn a healthy Python gate into EAGAIN.
_prepush_gate_seq=0
run_gate_captured() {
    if [ -z "${PREPUSH_GATE_LOG_DIR:-}" ] || [ ! -d "$PREPUSH_GATE_LOG_DIR" ]; then
        echo "[pre-push] gate capture directory is unavailable" >&2
        return 2
    fi

    _prepush_gate_seq=$((_prepush_gate_seq + 1))
    local gate_log="$PREPUSH_GATE_LOG_DIR/gate-${_prepush_gate_seq}.log"
    "$@" >"$gate_log" 2>&1
    local gate_status=$?
    cat "$gate_log" >&2 || true
    return "$gate_status"
}
