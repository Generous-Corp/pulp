#!/usr/bin/env bash
# Unit test for hooks/scripts/tool-registry-reminder.sh — the PostToolUse hook
# that catches an agent hand-rolling a tool Pulp already ships.
#
# Mirrors test_inject_claude_prefs_hook.sh's case-driven shell-test pattern.
# Each case feeds the hook a synthetic tool payload (via TOOL_INPUT or the
# stdin envelope) and asserts on stdout.
#
# Usage:  bash test/test_tool_registry_reminder_hook.sh
#
# Two invariants matter most:
#   * it FIRES on the real 2026-07 incident's signature (an agent writing a PIL
#     crop script to compare a render against its design source);
#   * it stays SILENT inside the registered tools themselves, which legitimately
#     import PIL — a hook that nags on every edit to montage.py gets ignored,
#     and an ignored hook is worse than no hook.
# It is advisory and must ALWAYS exit 0; a non-zero exit would start failing
# tool calls.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOOK="$SCRIPT_DIR/hooks/scripts/tool-registry-reminder.sh"

[ -x "$HOOK" ] || { echo "FAIL: hook missing or not executable: $HOOK"; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

# Run the hook with a TOOL_INPUT payload; capture stdout, assert exit 0.
run_env() {
    local out rc
    set +e
    out=$(TOOL_INPUT="$1" bash "$HOOK" 2>/dev/null </dev/null)
    rc=$?
    set -e
    [ "$rc" -eq 0 ] || fail "hook exited $rc (must always exit 0)"
    printf '%s' "$out"
}

# Run the hook with the payload on stdin (the hook-envelope channel).
run_stdin() {
    local out rc
    set +e
    out=$(printf '%s' "$1" | bash "$HOOK" 2>/dev/null)
    rc=$?
    set -e
    [ "$rc" -eq 0 ] || fail "hook exited $rc (must always exit 0)"
    printf '%s' "$out"
}

# ── The incident: a hand-rolled PIL comparison script ────────────────────────
out=$(run_env '{"file_path":"/tmp/compare_crops.py","content":"from PIL import Image\nref = Image.open(\"ref.png\")\nref.crop((0,0,100,100))"}')
case "$out" in
    *"REGISTERED TOOL"*) : ;;
    *) fail "hook did not fire on a hand-rolled PIL script (the real incident)" ;;
esac
case "$out" in
    *fidelity_diff.py*) : ;;
    *) fail "reminder must name fidelity_diff.py" ;;
esac
case "$out" in
    *montage.py*) : ;;
    *) fail "reminder must name montage.py" ;;
esac
case "$out" in
    *docs/status/tools.yaml*) : ;;
    *) fail "reminder must point at the registry" ;;
esac
pass "fires on a hand-rolled PIL script and names the registered tools"

# ── Edit (new_string) carries the signal too ─────────────────────────────────
out=$(run_env '{"file_path":"/tmp/x.py","old_string":"pass","new_string":"import PIL.Image as I"}')
case "$out" in
    *"REGISTERED TOOL"*) : ;;
    *) fail "hook did not fire on an Edit new_string" ;;
esac
pass "fires on an Edit's new_string"

# ── Bash `python3 -c` with an image-diff idiom, via the stdin envelope ───────
out=$(run_stdin '{"tool_name":"Bash","tool_input":{"command":"python3 -c \"from PIL import ImageChops; print(1)\""}}')
case "$out" in
    *"REGISTERED TOOL"*) : ;;
    *) fail "hook did not fire on a Bash python3 -c image diff" ;;
esac
pass "fires on a Bash python3 -c image diff (stdin envelope)"

# ── MUST be silent inside the registered tools (they own PIL legitimately) ───
for f in \
    /Users/dev/Code/pulp/tools/import-design/montage.py \
    /Users/dev/Code/pulp/tools/import-design/fidelity_diff.py \
    /Users/dev/Code/pulp/tools/import-validation/golden_regression.py \
    /Users/dev/Code/pulp/tools/scripts/figma_import_diff.py \
    /Users/dev/Code/pulp/tools/harness/visual/differ.py
do
    out=$(run_env "{\"file_path\":\"$f\",\"content\":\"from PIL import Image, ImageChops\"}")
    [ -z "$out" ] || fail "hook must stay silent inside a registered tool: $f"
done
pass "silent inside the registered tools themselves"

# ── MUST be silent on unrelated work ────────────────────────────────────────
out=$(run_env '{"file_path":"/tmp/foo.cpp","content":"int main() { return 0; }"}')
[ -z "$out" ] || fail "hook fired on an unrelated C++ edit"
out=$(run_env '{"file_path":"/tmp/notes.md","content":"Just some prose about an image."}')
[ -z "$out" ] || fail "hook fired on prose mentioning an image"
pass "silent on unrelated edits"

# ── Hand-rolled audio measurement ───────────────────────────────────────────
out=$(run_env '{"file_path":"/tmp/measure.py","content":"import soundfile\nx, sr = soundfile.read(\"out.wav\")"}')
case "$out" in
    *"REGISTERED TOOL"*"pulp audio validate"*) : ;;
    *) fail "hook did not point a hand-rolled WAV read at pulp audio validate" ;;
esac
pass "fires on hand-rolled audio measurement"

# ── Robustness: never blows up, never blocks ────────────────────────────────
out=$(run_env 'not json at all')
[ -z "$out" ] || fail "hook emitted output for malformed JSON"
out=$(run_env '')
[ -z "$out" ] || fail "hook emitted output for an empty payload"
out=$(run_env '{"file_path":"/tmp/a.py"}')
[ -z "$out" ] || fail "hook emitted output for a payload with no body"
pass "silent + exit 0 on malformed, empty, and bodyless payloads"

echo "PASS: tool-registry-reminder hook"
