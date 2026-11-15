#!/usr/bin/env bash

# Per-push receipt (tools/scripts/prepush_receipt.py). The hook records what it
# observed in an events file as it goes and writes one JSON line on exit, so a
# CI failure can later be traced to a skipped, demoted or missed local gate.
# Nothing here may change the hook's exit status or fail a push.
PREPUSH_RECEIPT_EVENTS="$(mktemp "${TMPDIR:-/tmp}/pulp-prepush-receipt.XXXXXX" 2>/dev/null || true)"

# prepush_receipt_note ran|skipped|demoted GATE DETAIL
prepush_receipt_note() {
    [ -n "${PREPUSH_RECEIPT_EVENTS:-}" ] || return 0
    printf '%s\t%s\t%s\n' "$1" "$2" "${3:-}" >>"$PREPUSH_RECEIPT_EVENTS" 2>/dev/null || true
}

# Called from the EXIT trap with the hook's exit status.
prepush_receipt_finish() {
    local rc="${1:-0}"
    local writer="${ROOT:-}/tools/scripts/prepush_receipt.py"
    if [ -f "$writer" ] && [ -n "${PULP_PYTHON:-}" ]; then
        printf '%s\n' "${PREPUSH_UPDATE_RECORDS:-}" | \
            "$PULP_PYTHON" "$writer" --root "$ROOT" --events "${PREPUSH_RECEIPT_EVENTS:-}" \
                --exit-code "$rc" --base "${BASE:-${PULP_PREPUSH_BASE:-origin/main}}" \
                --gate-host "${_prepush_gate_host:-0}" >&2 \
            || echo "[pre-push] receipt not written (writer failed)." >&2
    else
        echo "[pre-push] receipt not written (writer or interpreter missing)." >&2
    fi
    [ -z "${PREPUSH_RECEIPT_EVENTS:-}" ] || rm -f -- "$PREPUSH_RECEIPT_EVENTS" 2>/dev/null || true
    return 0
}
