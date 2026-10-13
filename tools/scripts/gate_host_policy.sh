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

pulp_gate_host_profile_value() {
    awk -F= -v key="$1" '$1 == key { sub(/^[^=]*=/, ""); print; exit }' <<<"$2"
}

# Every fleet host reports TARTCI_SERVES_GATE=1, so that field alone cannot
# pick the hosts to protect. TARTCI_ROLE does: `light` hosts lend spare
# capacity to the gate and keep room for local suites, while
# `dedicated-builder` and `dev-overflow` hosts carry the required gate VMs and
# a local suite competes with them. An unknown role on a gate-serving host is
# treated as reserved, so a new role fails closed. No tartci, or no profile,
# means this is not a fleet host.
pulp_gate_host_builds_reserved() {
    local profile role
    profile="$(pulp_gate_host_profile)" || return 1
    [ "$(pulp_gate_host_profile_value TARTCI_SERVES_GATE "$profile")" = "1" ] || return 1
    role="$(pulp_gate_host_profile_value TARTCI_ROLE "$profile")"
    PULP_GATE_HOST_ROLE="${role:-unknown}"
    [ "$role" != "light" ]
}

pulp_gate_host_skip_line() {
    local skipped="$1"
    echo "[gate-host] REFUSED ${skipped}: tartci host-profile says this host serves the required gate (TARTCI_ROLE=${PULP_GATE_HOST_ROLE:-unknown}), and a local suite here starves its gate VMs. Run ${2:-tools/scripts/gates.sh origin/main} from a Pulp worktree on a TARTCI_ROLE=light host (m1) or a disposable VM; CI runs these suites on the PR either way." >&2
}
