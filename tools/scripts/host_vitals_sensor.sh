#!/usr/bin/env bash
# host_vitals_sensor.sh — one tick of the host-vitals sensor, run on an interval
# by launchd (com.pulp.host-vitals). It probes host_vitals.sh, appends a
# timestamped line to a rotating log, and publishes the latest reading to a
# well-known JSON state file that other tools read to back off:
#
#     ~/.local/state/pulp/host_vitals.json   (latest reading, atomically swapped)
#     ~/.local/state/pulp/host_vitals.log    (history, rotated at ~2 MB)
#
# It is PURELY OBSERVATIONAL: it never stops a runner, kills a process, or shells
# any load. That keeps its blast radius at zero on a required-gate CI host — the
# worst it can do is write a log line. The active back-off decisions live in the
# CONSUMERS (the agent loop refusing new builds, the tartci pool draining,
# Shipyard parking a dispatch), each of which reads this state file. Staging the
# sensor ahead of any active shedding mirrors the reap-only rollout discipline
# used for the ship-queue janitor.
#
# The publish interval (60 s via the plist) samples the ~20-minute escalation
# that precedes a memory-exhaustion reboot roughly 20 times, so a consumer has
# ample warning to back off before jetsam.
set -u

STATE_DIR="${PULP_VITALS_STATE_DIR:-$HOME/.local/state/pulp}"
mkdir -p "$STATE_DIR"

HV="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/host_vitals.sh"
if [ ! -x "$HV" ]; then
    echo "host_vitals_sensor: host_vitals.sh not found next to sensor ($HV)" >&2
    exit 2
fi

json="$("$HV" --json 2>/dev/null)"; code=$?
ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

printf '%s %s\n' "$ts" "$json" >> "$STATE_DIR/host_vitals.log"

# Atomic publish so a reader never sees a half-written file.
printf '%s\n' "$json" > "$STATE_DIR/host_vitals.json.tmp" \
    && mv -f "$STATE_DIR/host_vitals.json.tmp" "$STATE_DIR/host_vitals.json"

# Rotate the history log once it passes ~2 MB (keep the last 1000 lines).
if [ -f "$STATE_DIR/host_vitals.log" ]; then
    sz="$(stat -f %z "$STATE_DIR/host_vitals.log" 2>/dev/null || echo 0)"
    if [ "$sz" -gt 2097152 ]; then
        tail -1000 "$STATE_DIR/host_vitals.log" > "$STATE_DIR/host_vitals.log.tmp" \
            && mv -f "$STATE_DIR/host_vitals.log.tmp" "$STATE_DIR/host_vitals.log"
    fi
fi

# Non-green readings also go to stderr so launchd captures them in the agent log.
if [ "$code" -ge 10 ]; then
    echo "host_vitals_sensor: $json" >&2
fi

exit "$code"
