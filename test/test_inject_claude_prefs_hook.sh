#!/usr/bin/env bash
# Unit test for hooks/scripts/inject-claude-prefs.sh — the SessionStart hook
# that reads `[claude] send_user_file` from the Pulp config and injects the
# SendUserFile preference into the agent's context.
#
# Mirrors test_check_pulp_cli_hook.sh's case-driven shell-test pattern. Each
# case stages a config file and points the hook at it via PULP_CONFIG_FILE so
# the states (default-on / explicit on / off / malformed / unset) run
# deterministically without touching ~/.pulp.
#
# Usage:  bash test/test_inject_claude_prefs_hook.sh
#
# The hook is informational and must ALWAYS exit 0 — a non-zero exit would
# start blocking Claude Code session init, a real regression.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOOK="$SCRIPT_DIR/hooks/scripts/inject-claude-prefs.sh"

[ -x "$HOOK" ] || { echo "FAIL: hook missing or not executable: $HOOK"; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Assert stdout is the injection JSON with a non-empty additionalContext and
# the enabling hint. Uses python3 (available in CI) for strict JSON parsing.
assert_injects() {
    local out="$1" label="$2"
    [ -n "$out" ] || fail "$label: expected injection output, got empty"
    printf '%s' "$out" | python3 -c '
import sys, json
d = json.load(sys.stdin)
hs = d["hookSpecificOutput"]
assert hs["hookEventName"] == "SessionStart", hs
ctx = hs["additionalContext"]
assert "SendUserFile" in ctx, "missing SendUserFile mention"
assert "send_user_file" in ctx, "missing toggle hint"
' || fail "$label: stdout was not the expected SessionStart injection JSON"
}

run_hook() {  # $1 = config file path (may be nonexistent)
    PULP_CONFIG_FILE="$1" bash "$HOOK"
}

# ── Case 1: config file does not exist → default ON (injects) ───────────────
out=$(run_hook "$tmp/missing.toml") || fail "case1: hook exited non-zero"
assert_injects "$out" "case1 (unset file)"
pass "missing config file → defaults to ON and injects"

# ── Case 2: [claude] send_user_file = "off" → no injection ─────────────────
printf '[claude]\nsend_user_file = "off"\n' > "$tmp/off.toml"
out=$(run_hook "$tmp/off.toml") || fail "case2: hook exited non-zero"
[ -z "$out" ] || fail "case2: expected no output when off, got: $out"
pass "send_user_file = off → no injection"

# ── Case 3: explicit on, with other sections + a trailing comment ──────────
printf '[pr]\nworkflow = "github"\n\n[claude]\nsend_user_file = "on"  # embed\n' > "$tmp/on.toml"
out=$(run_hook "$tmp/on.toml") || fail "case3: hook exited non-zero"
assert_injects "$out" "case3 (explicit on)"
pass "send_user_file = on (amid other sections/comment) → injects"

# ── Case 4: [claude] present but key absent → default ON ───────────────────
printf '[claude]\nother_key = "x"\n' > "$tmp/partial.toml"
out=$(run_hook "$tmp/partial.toml") || fail "case4: hook exited non-zero"
assert_injects "$out" "case4 (key absent)"
pass "key absent within [claude] → defaults to ON and injects"

# ── Case 5: key set OFF but in a DIFFERENT section → ignored, default ON ────
# Guards the section-scoping: a send_user_file under [pr] must not disable it.
printf '[pr]\nsend_user_file = "off"\n[claude]\nworkflow = "x"\n' > "$tmp/wrongsection.toml"
out=$(run_hook "$tmp/wrongsection.toml") || fail "case5: hook exited non-zero"
assert_injects "$out" "case5 (off under wrong section)"
pass "send_user_file=off under a non-claude section is ignored (stays ON)"

echo "all inject-claude-prefs hook cases passed"
