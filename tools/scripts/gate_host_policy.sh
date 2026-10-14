#!/usr/bin/env bash
# Shared policy for expensive local gates on tartci hosts that serve the gate.
# Host identity comes from tartci's profile, never from the macOS hostname.

# The tartci binary, or nothing. tartci installs to ~/.local/bin, which a
# launchd job, a sandboxed agent shell or a stripped-PATH hook may not have
# on PATH, so that location is checked too. PULP_TARTCI_BIN is a test hook:
# it names a fake profile, and one that does not exist is ignored rather than
# read as "no tartci", so it cannot hide the real binary.
pulp_gate_host_tartci() {
    local candidate
    for candidate in "${PULP_TARTCI_BIN:-}" "$(command -v tartci 2>/dev/null || true)" \
            "${HOME:+$HOME/.local/bin/tartci}"; do
        if [ -n "$candidate" ] && [ -x "$candidate" ] && [ ! -d "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

# Prints the profile. Returns 1 when there is no tartci (not a fleet host) and
# 2 when a tartci exists but its profile cannot be read.
pulp_gate_host_profile() {
    local tartci_bin profile
    tartci_bin="$(pulp_gate_host_tartci)" || return 1
    profile="$("$tartci_bin" host-profile 2>/dev/null)" || return 2
    printf '%s\n' "$profile"
}

pulp_gate_host_profile_value() {
    awk -F= -v key="$1" '$1 == key { sub(/^[^=]*=/, ""); print; exit }' <<<"$2"
}

# Every fleet host reports TARTCI_SERVES_GATE=1, so that field alone cannot
# pick the hosts to protect. TARTCI_ROLE does: `light` hosts lend spare
# capacity to the gate and keep room for local suites, while
# `dedicated-builder` and `dev-overflow` hosts carry the required gate VMs and
# a local suite competes with them. It fails closed: an unknown role, a
# profile without TARTCI_SERVES_GATE, or a tartci whose host-profile fails is
# treated as reserved. Only the absence of tartci means "not a fleet host".
pulp_gate_host_builds_reserved() {
    local profile rc serves role
    profile="$(pulp_gate_host_profile)"
    rc=$?
    if [ "$rc" -eq 1 ]; then
        return 1
    fi
    if [ "$rc" -ne 0 ]; then
        PULP_GATE_HOST_ROLE="unknown: tartci host-profile failed"
        return 0
    fi
    serves="$(pulp_gate_host_profile_value TARTCI_SERVES_GATE "$profile")"
    case "$serves" in
        0) return 1 ;;
        1) ;;
        *)
            PULP_GATE_HOST_ROLE="unknown: profile has no TARTCI_SERVES_GATE"
            return 0 ;;
    esac
    role="$(pulp_gate_host_profile_value TARTCI_ROLE "$profile")"
    PULP_GATE_HOST_ROLE="${role:-unknown}"
    [ "$role" != "light" ]
}

pulp_gate_host_skip_line() {
    local skipped="$1"
    echo "[gate-host] REFUSED ${skipped}: tartci host-profile says this host serves the required gate (TARTCI_ROLE=${PULP_GATE_HOST_ROLE:-unknown}), and a local suite here starves its gate VMs. Run ${2:-tools/scripts/gates.sh origin/main} from a Pulp worktree on a TARTCI_ROLE=light host (m1) or a disposable VM; CI runs these suites on the PR either way." >&2
}
