#!/usr/bin/env bash
# Shared policy for expensive local gates on tartci hosts that serve the gate.
# Host identity comes from tartci's profile, never from the macOS hostname.

pulp_gate_host_profile() {
    local tartci_bin="${PULP_TARTCI_BIN:-}"
    if [ -z "$tartci_bin" ]; then
        tartci_bin="$(command -v tartci 2>/dev/null || true)"
    fi
    [ -n "$tartci_bin" ] && [ -x "$tartci_bin" ] || return 1
    "$tartci_bin" host-profile 2>/dev/null
}

pulp_gate_host_serves_gate() {
    local profile
    profile="$(pulp_gate_host_profile)" || return 1
    awk -F= '$1 == "TARTCI_SERVES_GATE" && $2 == "1" { found=1 } END { exit(found ? 0 : 1) }' <<<"$profile"
}

pulp_gate_host_skip_line() {
    local skipped="$1"
    echo "[gate-host] skipped ${skipped}: tartci host-profile reports TARTCI_SERVES_GATE=1; in an active Pulp worktree on m1 or a disposable VM run: ${2:-tools/scripts/gates.sh origin/main}" >&2
}
