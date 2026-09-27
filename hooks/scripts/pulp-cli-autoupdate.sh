#!/usr/bin/env bash
# pulp-cli-autoupdate.sh — keep the installed `pulp` at the latest release.
#
# Called from the SessionStart hook (check-pulp-cli.sh --session-start). The
# foreground part only reads a cache and returns at once; the network check and
# the install run in a detached background job, at most once per interval.
#
#   1. Resolve the latest release tag from the github.com redirect of
#      /releases/latest (no REST API, so no rate-limit 403).
#   2. If the installed CLI is older, fetch install.sh AT THAT TAG, and run it
#      pinned to that version (PULP_VERSION), into the same directory, without
#      touching shell profiles or the SDK.
#   3. Append one line to the log saying what happened.
#
# Only an installer-managed CLI is updated: `pulp` on PATH must be
# $HOME/.pulp/bin/pulp. An installer that still leaves the WebGPU runtime to the
# optional broker transaction is never run, since it can strand pulp-cpp.
#
# Environment:
#   PULP_AUTO_UPDATE_CLI=0            opt out
#   PULP_AUTO_UPDATE_INTERVAL_SECS    check interval (default 21600 = 6 h)
#   PULP_AUTO_UPDATE_STATE_DIR        state/log dir (default ~/.pulp/state)
#   PULP_AUTO_UPDATE_FOREGROUND=1     run the job inline (tests)
#   PULP_AUTO_UPDATE_RELEASES_URL     release page base (tests)
#   PULP_AUTO_UPDATE_RAW_URL          raw-file base (tests)
#
# Output: at most one line on stdout, the newest log line not yet shown, so
# the session learns what the last background run did. Always exits 0.

set -u

[ "${PULP_AUTO_UPDATE_CLI:-1}" = "0" ] && exit 0
[ -n "${CI:-}" ] && exit 0
[ -n "${GITHUB_ACTIONS:-}" ] && exit 0

state_dir="${PULP_AUTO_UPDATE_STATE_DIR:-$HOME/.pulp/state}"
interval="${PULP_AUTO_UPDATE_INTERVAL_SECS:-21600}"
case "$interval" in ''|*[!0-9]*) interval=21600 ;; esac
releases_url="${PULP_AUTO_UPDATE_RELEASES_URL:-https://github.com/Generous-Corp/pulp/releases}"
raw_url="${PULP_AUTO_UPDATE_RAW_URL:-https://raw.githubusercontent.com/Generous-Corp/pulp}"
log="$state_dir/cli-autoupdate.log"
stamp="$state_dir/cli-autoupdate.last-check"
shown="$state_dir/cli-autoupdate.last-shown"
lock="$state_dir/cli-autoupdate.lock"

mkdir -p "$state_dir" 2>/dev/null || exit 0

# Report the newest outcome once.
if [ -s "$log" ]; then
    last="$(tail -n 1 "$log")"
    if [ "$last" != "$(cat "$shown" 2>/dev/null)" ]; then
        printf '[pulp] CLI auto-update: %s\n' "$last"
        printf '%s' "$last" > "$shown"
    fi
fi

cli_path="$(command -v pulp 2>/dev/null || true)"
[ "$cli_path" = "$HOME/.pulp/bin/pulp" ] || exit 0

now="$(date +%s)"
last_check="$(cat "$stamp" 2>/dev/null || echo 0)"
case "$last_check" in ''|*[!0-9]*) last_check=0 ;; esac
[ $((now - last_check)) -ge "$interval" ] || exit 0

# One job per host; a lock older than an hour is left over from a killed run.
if ! mkdir "$lock" 2>/dev/null; then
    if [ -n "$(find "$lock" -maxdepth 0 -mmin +60 2>/dev/null)" ]; then
        rmdir "$lock" 2>/dev/null && mkdir "$lock" 2>/dev/null || exit 0
    else
        exit 0
    fi
fi
printf '%s' "$now" > "$stamp"

triple() { grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n 1; }

# Exit 0 when $1 is a newer M.N.P than $2.
newer() {
    local IFS=.
    # shellcheck disable=SC2086
    set -- $1 $2
    [ "$1" -ne "$4" ] && { [ "$1" -gt "$4" ]; return; }
    [ "$2" -ne "$5" ] && { [ "$2" -gt "$5" ]; return; }
    [ "$3" -gt "$6" ]
}

note() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$1" >> "$log"; }

update_job() {
    trap 'rmdir "$lock" 2>/dev/null' EXIT
    local latest installed installer
    latest="$(curl -fsSI --max-time 15 "$releases_url/latest" 2>/dev/null |
        awk 'tolower($1) == "location:" { print $2 }' | tr -d '\r' | triple)"
    if [ -z "$latest" ]; then
        note "could not resolve the latest release (offline?); will retry next interval"
        return
    fi
    installed="$("$cli_path" version 2>/dev/null | head -n 1 | triple)"
    if [ -z "$installed" ] || ! newer "$latest" "$installed"; then
        return
    fi
    installer="$state_dir/install-v$latest.sh"
    if ! curl -fsSL --max-time 30 "$raw_url/v$latest/tools/install/install.sh" -o "$installer"; then
        note "v$installed -> v$latest: could not fetch that release's installer"
        return
    fi
    if grep -q -- "--exclude='libwgpu_native.dylib'" "$installer"; then
        note "v$installed -> v$latest: skipped, that release's installer can strand pulp-cpp without its runtime"
        rm -f "$installer"
        return
    fi
    if PULP_VERSION="$latest" PULP_INSTALL_DIR="$HOME/.pulp/bin" PULP_NO_MODIFY_PATH=1 \
        PULP_SKIP_SDK_INSTALL=1 bash "$installer" > "$state_dir/cli-autoupdate.install.log" 2>&1 &&
        [ "$("$cli_path" version 2>/dev/null | head -n 1 | triple)" = "$latest" ]; then
        note "updated v$installed -> v$latest"
    else
        note "v$installed -> v$latest failed; see $state_dir/cli-autoupdate.install.log"
    fi
    rm -f "$installer"
}

if [ "${PULP_AUTO_UPDATE_FOREGROUND:-0}" = "1" ]; then
    update_job
else
    ( update_job ) </dev/null >/dev/null 2>&1 &
    disown 2>/dev/null || true
fi
exit 0
