#!/bin/bash
# heartbeat_wait.sh — run a long, silent command while proving it is alive.
#
# Source this; it defines one function and runs nothing on its own.
#
# WHY THIS EXISTS
# ---------------
# `xcrun notarytool submit --wait` writes its entire poll phase as ONE
# unterminated line, flushed only when Apple returns a verdict, and stdio
# switches to full buffering the moment stdout is a file rather than a TTY.
# A watcher tailing a redirected packaging log therefore sees nothing at all
# between "Submission ID received" and the verdict — a window that is routinely
# 5 to 30 minutes on every notarized release.
#
# The only other instrument reachable from outside is the process name, and
# that one is blind too: the packaging wrappers (examples/*/package.sh) end by
# `exec`ing into build_combined_installer.sh, so `pgrep -f package.sh` returns
# 0 for a live run from a few seconds in. Two plausible probes, both reading
# "dead" on a healthy pipeline.
#
# So the liveness signal has to come from the recipe itself. This is the same
# answer the repo already bought once for the pre-push diff-coverage build,
# which prints a banner plus an "(Ns elapsed)" heartbeat for exactly this
# reason ("a slow `git push` is almost always this build — NOT the network").
#
# WHAT IT MUST NOT DO
# -------------------
# The exit status of the wrapped command stays authoritative. Under
# `set -euo pipefail` a failing notarization must still abort before
# `stapler staple`, so an un-notarized .pkg can never reach the staple /
# validate / "OK →" path. That is why the real command runs in the FOREGROUND
# and only the heartbeat is backgrounded: the status is this function's own
# return value, never routed through a `wait` on the job under measurement.

# pulp_run_with_heartbeat <label> <interval_secs> <command> [args...]
#
# Runs the command in the foreground, printing
#   [heartbeat] <label> in progress (<N>s elapsed) — not hung
# to stdout every <interval_secs> until it returns. Returns the command's exit
# status unchanged. The command's own stdout/stderr pass straight through.
pulp_run_with_heartbeat() {
    if [ "$#" -lt 3 ]; then
        echo "pulp_run_with_heartbeat: usage: <label> <interval_secs> <command> [args...]" >&2
        return 2
    fi
    local _hb_label="$1" _hb_interval="$2"
    shift 2
    case "$_hb_interval" in
        ''|*[!0-9]*|0) echo "pulp_run_with_heartbeat: interval must be a positive integer" >&2; return 2 ;;
    esac

    # $$ is the sourcing shell's pid even inside this subshell, so the
    # heartbeat self-terminates if the recipe is killed outright rather than
    # outliving it and printing into a log nobody owns.
    local _hb_parent=$$
    (
        _t=0
        while :; do
            sleep "$_hb_interval"
            kill -0 "$_hb_parent" 2>/dev/null || exit 0
            _t=$(( _t + _hb_interval ))
            printf '[heartbeat] %s in progress (%ss elapsed) — not hung\n' "$_hb_label" "$_t"
        done
    ) &
    local _hb_pid=$!

    local _hb_rc=0
    "$@" || _hb_rc=$?

    kill "$_hb_pid" 2>/dev/null || true
    wait "$_hb_pid" 2>/dev/null || true
    return "$_hb_rc"
}
