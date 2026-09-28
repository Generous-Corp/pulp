#!/usr/bin/env bash
# Hermetic coverage for hooks/scripts/pulp-cli-autoupdate.sh.
#
# A throwaway HOME holds a stub installed `pulp`; stub `curl` answers the
# /releases/latest redirect and serves a fixture installer per tag, so each
# case exercises the real script without touching the network or ~/.pulp.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOOK="$SCRIPT_DIR/hooks/scripts/pulp-cli-autoupdate.sh"
ROOT="$(mktemp -d)"
trap 'rm -rf "$ROOT"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS — $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL — $1"; FAIL=$((FAIL + 1)); }

# new_case <name> <installed> <latest> <installer: good|stranding>
new_case() {
    CASE="$ROOT/$1"
    HOME_DIR="$CASE/home"
    mkdir -p "$HOME_DIR/.pulp/bin" "$CASE/mock"
    printf '%s\n' "$2" > "$HOME_DIR/installed-version"
    cat > "$HOME_DIR/.pulp/bin/pulp" <<'EOF'
#!/bin/sh
[ "${1:-}" = version ] && echo "pulp v$(cat "$HOME/installed-version")"
EOF
    chmod +x "$HOME_DIR/.pulp/bin/pulp"
    if [ "$4" = good ]; then
        cat > "$CASE/installer.sh" <<'EOF'
#!/bin/bash
printf 'installer|version=%s|dir=%s|nopath=%s|nosdk=%s\n' "$PULP_VERSION" \
    "$PULP_INSTALL_DIR" "$PULP_NO_MODIFY_PATH" "$PULP_SKIP_SDK_INSTALL" >> "$HOME/calls.log"
printf '%s\n' "$PULP_VERSION" > "$HOME/installed-version"
EOF
    else
        cat > "$CASE/installer.sh" <<'EOF'
#!/bin/bash
        tar --exclude='libwgpu_native.dylib' -xzf pulp.tar.gz
printf '%s\n' "$PULP_VERSION" > "$HOME/installed-version"
EOF
    fi
    cat > "$CASE/mock/curl" <<EOF
#!/bin/sh
echo "curl \$*" >> "$HOME_DIR/calls.log"
[ -n "\${MOCK_CURL_SLEEP:-}" ] && sleep "\$MOCK_CURL_SLEEP"
out=
head=0
url=
while [ "\$#" -gt 0 ]; do
    case "\$1" in
        -o) shift; out="\$1" ;;
        -fsSI) head=1 ;;
        -*) ;;
        *) url="\$1" ;;
    esac
    shift
done
if [ "\$head" = 1 ]; then
    printf 'HTTP/2 302\r\nlocation: https://example.invalid/releases/tag/v$3\r\n\r\n'
    exit 0
fi
case "\$url" in
    */v$3/tools/install/install.sh) cp "$CASE/installer.sh" "\$out" ;;
    *) exit 22 ;;
esac
EOF
    chmod +x "$CASE/mock/curl"
}

run_hook() {
    env -u CI -u GITHUB_ACTIONS -u PULP_AUTO_UPDATE_CLI \
        HOME="$HOME_DIR" \
        PATH="$HOME_DIR/.pulp/bin:$CASE/mock:/usr/bin:/bin" \
        PULP_AUTO_UPDATE_FOREGROUND="${FOREGROUND:-1}" \
        PULP_AUTO_UPDATE_RELEASES_URL=https://example.invalid/releases \
        PULP_AUTO_UPDATE_RAW_URL=https://raw.example.invalid \
        "$@" bash "$HOOK"
}

echo "Case: installed CLI behind the latest release"
new_case behind 0.876.1 0.877.0 good
run_hook > "$CASE/out1"
if grep -qF "installer|version=0.877.0|dir=$HOME_DIR/.pulp/bin|nopath=1|nosdk=1" "$HOME_DIR/calls.log"; then
    pass "runs that release's installer pinned to its version, CLI only"
else
    fail "installer call wrong: $(cat "$HOME_DIR/calls.log" 2>/dev/null)"
fi
if grep -q "api.github.com" "$HOME_DIR/calls.log"; then
    fail "must not use the REST API (rate-limited)"
else
    pass "resolves the release without the REST API"
fi
first="$(cat "$CASE/out1")"
[ -z "$first" ] && pass "the first run has nothing old to report" ||
    fail "the first run reported: $first"
if [ "$(grep -c . "$HOME_DIR/.pulp/state/cli-autoupdate.log")" = 1 ] &&
    grep -q "updated v0.876.1 -> v0.877.0" "$HOME_DIR/.pulp/state/cli-autoupdate.log"; then
    pass "logs one line per update"
else
    fail "log: $(cat "$HOME_DIR/.pulp/state/cli-autoupdate.log" 2>/dev/null)"
fi
out="$(run_hook PULP_AUTO_UPDATE_INTERVAL_SECS=0)"
case "$out" in
    "[pulp] CLI auto-update: "*"updated v0.876.1 -> v0.877.0") pass "reports the last outcome to the next session" ;;
    *) fail "session report missing: $out" ;;
esac
[ "$(grep -c '^installer' "$HOME_DIR/calls.log")" = 1 ] && pass "an updated CLI is not reinstalled" ||
    fail "reinstalled: $(cat "$HOME_DIR/calls.log")"
out="$(run_hook PULP_AUTO_UPDATE_INTERVAL_SECS=0)"
[ -z "$out" ] && pass "reports each outcome once" || fail "repeated report: $out"

echo "Case: checked within the interval"
new_case fresh 0.876.1 0.877.0 good
mkdir -p "$HOME_DIR/.pulp/state"
date +%s > "$HOME_DIR/.pulp/state/cli-autoupdate.last-check"
run_hook > /dev/null
[ ! -e "$HOME_DIR/calls.log" ] && pass "no network inside the 6 h interval" ||
    fail "checked again inside the interval: $(cat "$HOME_DIR/calls.log")"

echo "Case: opted out"
new_case optout 0.876.1 0.877.0 good
run_hook PULP_AUTO_UPDATE_CLI=0 > /dev/null
[ ! -e "$HOME_DIR/calls.log" ] && pass "PULP_AUTO_UPDATE_CLI=0 does nothing" || fail "ran while opted out"

echo "Case: the release's installer still strands the runtime"
new_case stranding 0.876.1 0.877.0 stranding
run_hook > /dev/null
if [ "$(cat "$HOME_DIR/installed-version")" = 0.876.1 ] &&
    grep -q "skipped, that release's installer can strand pulp-cpp" "$HOME_DIR/.pulp/state/cli-autoupdate.log"; then
    pass "refuses an installer that excludes the runtime"
else
    fail "ran a stranding installer: $(cat "$HOME_DIR/installed-version")"
fi

echo "Case: already current"
new_case current 0.877.0 0.877.0 good
run_hook > /dev/null
if ! grep -q "^installer" "$HOME_DIR/calls.log" && [ ! -s "$HOME_DIR/.pulp/state/cli-autoupdate.log" ]; then
    pass "a current CLI is left alone, silently"
else
    fail "touched a current CLI"
fi

echo "Case: pulp on PATH is not the installer-managed one"
new_case elsewhere 0.876.1 0.877.0 good
mkdir -p "$CASE/other"
cp "$HOME_DIR/.pulp/bin/pulp" "$CASE/other/pulp"
env -u CI -u GITHUB_ACTIONS HOME="$HOME_DIR" PATH="$CASE/other:$CASE/mock:/usr/bin:/bin" \
    PULP_AUTO_UPDATE_FOREGROUND=1 bash "$HOOK" > /dev/null
[ ! -e "$HOME_DIR/calls.log" ] && pass "never updates a CLI it did not install" || fail "updated a foreign CLI"

echo "Case: the session never waits for the network"
new_case background 0.876.1 0.877.0 good
start=$(date +%s)
FOREGROUND=0 run_hook MOCK_CURL_SLEEP=4 > /dev/null
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -le 2 ] && pass "returns in ${elapsed}s while the check runs in the background" ||
    fail "blocked the session for ${elapsed}s"
for _ in $(seq 1 60); do
    [ -d "$HOME_DIR/.pulp/state/cli-autoupdate.lock" ] || break
    sleep 0.2
done
[ "$(cat "$HOME_DIR/installed-version")" = 0.877.0 ] && pass "the background job completes the update" ||
    fail "background update did not complete"

echo ""
echo "Result: $PASS pass, $FAIL fail"
[ "$FAIL" -eq 0 ]
