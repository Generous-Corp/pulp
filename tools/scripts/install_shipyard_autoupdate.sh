#!/usr/bin/env bash
# install_shipyard_autoupdate.sh — install the Shipyard pin-converger as a
# per-user launchd agent, so a Mac in the CI fleet quietly keeps its
# installed Shipyard equal to the version pinned in tools/shipyard.toml
# without anyone having to remember.
#
# Installing this is opt-in, per machine. Pulp itself never needs it: a
# public cloner runs tools/install-shipyard.sh once and is done. This is
# fleet ergonomics for machines that ship PRs every day, so it lives
# outside the public contract and degrades to "nothing happens" when it
# is simply not installed.
#
# What it does on each tick (hourly by default):
#   * reads the pin from origin/main (not the parked branch)
#   * does nothing, silently, if the installed version already equals it
#   * defers if the host is mid-job (a Runner.Worker or a live ship)
#   * otherwise converges: `shipyard update --to <pin>` to move forward,
#     tools/install-shipyard.sh to come back from ahead of the pin
#   * leaves the working binary untouched on any failure
#
# Turn it off WITHOUT uninstalling (the kill switch):
#     echo off > ~/.config/pulp/shipyard-autoupdate
# Turn it back on:
#     echo on  > ~/.config/pulp/shipyard-autoupdate
# Remove it entirely:
#     tools/scripts/install_shipyard_autoupdate.sh --uninstall
#
# Usage:
#     tools/scripts/install_shipyard_autoupdate.sh            # install / refresh
#     tools/scripts/install_shipyard_autoupdate.sh --uninstall
#     tools/scripts/install_shipyard_autoupdate.sh --status
#     tools/scripts/install_shipyard_autoupdate.sh --dry-run  # print the plist
set -euo pipefail

LABEL="com.pulp.shipyard-autoupdate"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SRC_DIR/../.." && pwd)"
# The agent runs a COPY at a stable path, never the file inside a
# checkout: a worktree gets deleted when its PR lands, and a plist
# pointing into it would then fail silently forever. The copy still needs
# a real checkout for the pin and for the downgrade installer — that is
# the REPO below — but a missing repo surfaces as a loud config error,
# whereas a missing program is just a dead agent.
BIN_DIR="$HOME/.local/bin"
AGENT="$BIN_DIR/shipyard_autoupdate.py"
STATE_DIR="$HOME/.local/state/pulp"
LOG_DIR="$HOME/Library/Logs/pulp"
CONFIG="$HOME/.config/pulp/shipyard-autoupdate"
PLIST="$HOME/Library/LaunchAgents/${LABEL}.plist"
# Hourly. The pin moves a few times a month; the tick is a `--version`
# probe plus a `ps` scan, so it costs nothing and converges an offline or
# mid-job machine within an hour of it becoming reachable and idle.
INTERVAL="${PULP_SHIPYARD_AUTOUPDATE_INTERVAL:-3600}"

uid="$(id -u)"
domain="gui/${uid}"

# The checkout the agent reads the pin from, baked in at install time.
# Defaults to the checkout this script was run from, so install from the
# machine's long-lived clone — NOT from a throwaway worktree, whose
# deletion would leave the agent unable to find a pin.
REPO="${PULP_SHIPYARD_AUTOUPDATE_REPO:-$REPO_ROOT}"

status() {
    echo "== shipyard-autoupdate status =="
    if launchctl print "${domain}/${LABEL}" >/dev/null 2>&1; then
        echo "launchd:  LOADED (${domain}/${LABEL})"
    else
        echo "launchd:  not loaded"
    fi
    echo "repo:     $REPO"
    if [ -f "$CONFIG" ]; then
        echo "switch:   $CONFIG -> $(tr -d '\n' < "$CONFIG")"
    else
        echo "switch:   (absent — enabled; 'echo off > $CONFIG' to disable)"
    fi
    if [ -f "$STATE_DIR/shipyard_autoupdate.json" ]; then
        echo "last run: $(cat "$STATE_DIR/shipyard_autoupdate.json")"
    else
        echo "last run: (no reading yet)"
    fi
    echo "-- current pin vs installed --"
    probe="$AGENT"; [ -f "$probe" ] || probe="$SRC_DIR/shipyard_autoupdate.py"
    PULP_SHIPYARD_AUTOUPDATE_REPO="$REPO" \
        /usr/bin/python3 "$probe" --check --json 2>&1 || true
}

uninstall() {
    launchctl bootout "${domain}/${LABEL}" 2>/dev/null || true
    rm -f "$PLIST" "$AGENT"
    echo "shipyard-autoupdate uninstalled (Shipyard itself left exactly as-is)."
}

emit_plist() {
    cat <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>${LABEL}</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/bin/python3</string>
        <string>${AGENT}</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>PULP_SHIPYARD_AUTOUPDATE_REPO</key>
        <string>${REPO}</string>
        <!-- A launchd agent gets a minimal PATH and would not find
             shipyard at ~/.local/bin, nor git from the CLT shims. -->
        <key>PATH</key>
        <string>${HOME}/.local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin</string>
    </dict>
    <key>StartInterval</key>
    <integer>${INTERVAL}</integer>
    <!-- Deliberately no RunAtLoad: a login/reboot is exactly when the
         runners are starting work, and there is no reason to race them
         for a pin that will still be there an hour later. -->
    <key>StandardOutPath</key>
    <string>${LOG_DIR}/shipyard-autoupdate.out.log</string>
    <key>StandardErrorPath</key>
    <string>${LOG_DIR}/shipyard-autoupdate.err.log</string>
    <key>ProcessType</key>
    <string>Background</string>
    <key>LowPriorityIO</key>
    <true/>
    <key>Nice</key>
    <integer>10</integer>
</dict>
</plist>
PLIST_EOF
}

case "${1:-}" in
    --uninstall) uninstall; exit 0 ;;
    --status) status; exit 0 ;;
    --dry-run) emit_plist; exit 0 ;;
    ''|--install) : ;;
    *) echo "usage: $0 [--install|--uninstall|--status|--dry-run]" >&2; exit 2 ;;
esac

if [ ! -f "$REPO/tools/shipyard.toml" ]; then
    echo "Error: no tools/shipyard.toml under $REPO — point" >&2
    echo "PULP_SHIPYARD_AUTOUPDATE_REPO at a real Pulp checkout." >&2
    exit 1
fi

mkdir -p "$BIN_DIR" "$STATE_DIR" "$LOG_DIR" "$(dirname "$PLIST")" "$(dirname "$CONFIG")"
install -m 0755 "$SRC_DIR/shipyard_autoupdate.py" "$AGENT"
emit_plist > "$PLIST"

# Re-bootstrap so an updated plist takes effect immediately.
launchctl bootout "${domain}/${LABEL}" 2>/dev/null || true
launchctl bootstrap "${domain}" "$PLIST"

echo "shipyard-autoupdate installed on $(hostname -s): every ${INTERVAL}s, repo ${REPO}"
echo "  kill switch: echo off > $CONFIG"
echo "  uninstall:   $0 --uninstall"
echo ""
status
