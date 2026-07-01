#!/usr/bin/env bash
# install_host_vitals_sensor.sh — install the host-vitals sensor as a per-user
# launchd agent on a macOS CI host, so it publishes host health every 60 s to
# ~/.local/state/pulp/host_vitals.json for the agent loop / tartci pool /
# Shipyard to read.
#
# Idempotent: safe to re-run to update the scripts or refresh the agent. It
# copies host_vitals.sh + host_vitals_sensor.sh to ~/.local/bin (a stable path
# independent of any repo checkout), writes the LaunchAgent plist, and
# (re)bootstraps it. The sensor is observation-only, so this install can never
# take a CI runner offline.
#
# Usage:
#     tools/scripts/install_host_vitals_sensor.sh            # install / refresh
#     tools/scripts/install_host_vitals_sensor.sh --uninstall
#     tools/scripts/install_host_vitals_sensor.sh --status
set -euo pipefail

LABEL="com.pulp.host-vitals"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="$HOME/.local/bin"
STATE_DIR="$HOME/.local/state/pulp"
LOG_DIR="$HOME/Library/Logs/pulp"
PLIST="$HOME/Library/LaunchAgents/${LABEL}.plist"
INTERVAL="${PULP_VITALS_INTERVAL:-60}"

uid="$(id -u)"
domain="gui/${uid}"

uninstall() {
    launchctl bootout "${domain}/${LABEL}" 2>/dev/null || true
    rm -f "$PLIST"
    echo "host-vitals sensor uninstalled (scripts + state left in place)."
}

status() {
    echo "== host-vitals sensor status =="
    if launchctl print "${domain}/${LABEL}" >/dev/null 2>&1; then
        echo "launchd: LOADED (${domain}/${LABEL})"
    else
        echo "launchd: not loaded"
    fi
    if [ -f "$STATE_DIR/host_vitals.json" ]; then
        echo "latest: $(cat "$STATE_DIR/host_vitals.json")"
    else
        echo "latest: (no reading yet)"
    fi
}

case "${1:-}" in
    --uninstall) uninstall; exit 0 ;;
    --status) status; exit 0 ;;
    ''|--install) : ;;
    *) echo "usage: $0 [--install|--uninstall|--status]" >&2; exit 2 ;;
esac

mkdir -p "$BIN_DIR" "$STATE_DIR" "$LOG_DIR" "$(dirname "$PLIST")"
install -m 0755 "$SRC_DIR/host_vitals.sh" "$BIN_DIR/host_vitals.sh"
install -m 0755 "$SRC_DIR/host_vitals_sensor.sh" "$BIN_DIR/host_vitals_sensor.sh"

cat > "$PLIST" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>${LABEL}</string>
    <key>ProgramArguments</key>
    <array>
        <string>${BIN_DIR}/host_vitals_sensor.sh</string>
    </array>
    <key>StartInterval</key>
    <integer>${INTERVAL}</integer>
    <key>RunAtLoad</key>
    <true/>
    <key>StandardOutPath</key>
    <string>${LOG_DIR}/host-vitals.out.log</string>
    <key>StandardErrorPath</key>
    <string>${LOG_DIR}/host-vitals.err.log</string>
    <key>ProcessType</key>
    <string>Background</string>
    <key>LowPriorityIO</key>
    <true/>
</dict>
</plist>
PLIST_EOF

# Re-bootstrap so an updated plist / script takes effect immediately.
launchctl bootout "${domain}/${LABEL}" 2>/dev/null || true
launchctl bootstrap "${domain}" "$PLIST"
launchctl kickstart "${domain}/${LABEL}" 2>/dev/null || true

echo "host-vitals sensor installed on $(hostname -s): every ${INTERVAL}s -> ${STATE_DIR}/host_vitals.json"
sleep 1
status
