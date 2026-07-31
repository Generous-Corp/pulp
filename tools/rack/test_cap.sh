#!/usr/bin/env bash
# The time limit itself, on a machine pretending to have no coreutils.
#
# `cap` has two jobs and only one of them was ever exercised. It stops a
# command that runs too long -- easy to believe, easy to test. It also has to
# RETURN when the command finishes early, and that half was broken for as long
# as the shim existed: the killer subshell inherited stdout, so `$(cap 1500 …)`
# blocked reading a pipe the killer still held. Every capped step cost its full
# cap, on exactly the machines that need the shim.
#
# It never showed up here because this machine has coreutils, so `cap` is
# `timeout` and the fallback never runs. These tests hide both from PATH.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bad=0

ok()   { printf '  ok     %s\n' "$*"; }
wrong() { printf '  WRONG  %s\n' "$*"; bad=$((bad + 1)); }

# A PATH with no timeout and no gtimeout, so the fallback is what runs.
BARE="$(mktemp -d)"
trap 'rm -rf "$BARE"' EXIT
for tool in sh bash sleep kill env printf date; do
    src="$(command -v "$tool" 2>/dev/null)" && ln -sf "$src" "$BARE/$tool"
done
export PATH="/usr/bin:/bin"          # macOS's own PATH: no Homebrew, no coreutils

if command -v timeout >/dev/null 2>&1 || command -v gtimeout >/dev/null 2>&1; then
    wrong "the bare PATH still has a timeout — this test would prove nothing"
else
    ok "the fallback shim is what these tests exercise"
fi

# shellcheck source=cap.sh
. "$HERE/cap.sh"

# 1. A command that finishes fast must RETURN fast, with its output, when its
#    output is captured. This is the one that was broken.
start=$(date +%s)
out="$(cap 30 /bin/echo hello)"
elapsed=$(( $(date +%s) - start ))
if [ "$out" != "hello" ]; then
    wrong "captured output was '$out', not 'hello'"
elif [ "$elapsed" -ge 5 ]; then
    wrong "a command that took no time returned after ${elapsed}s — the cap is
         being waited out instead of the command"
else
    ok "a fast command returns in ${elapsed}s with its output"
fi

# 2. The same, uncaptured, so a regression cannot hide in one calling style.
start=$(date +%s)
cap 30 /bin/echo hello >/dev/null
elapsed=$(( $(date +%s) - start ))
if [ "$elapsed" -ge 5 ]; then
    wrong "uncaptured, a fast command still took ${elapsed}s"
else
    ok "uncaptured, a fast command returns in ${elapsed}s"
fi

# 3. The cap still caps. A shim that returns instantly by never limiting
#    anything would pass everything above.
start=$(date +%s)
cap 2 /bin/sleep 30 >/dev/null 2>&1
rc=$?
elapsed=$(( $(date +%s) - start ))
if [ "$elapsed" -ge 10 ]; then
    wrong "a 2s cap let a 30s command run for ${elapsed}s"
elif [ "$rc" -eq 0 ]; then
    wrong "a command that was killed reported success"
else
    ok "a 2s cap stops a 30s command after ${elapsed}s, non-zero"
fi

# 4. The exit status of a command that finishes on its own is its own.
cap 30 /bin/sh -c 'exit 7' >/dev/null 2>&1
rc=$?
if [ "$rc" -ne 7 ]; then
    wrong "exit status came back as $rc, not 7"
else
    ok "a command's own exit status survives the cap"
fi

printf '\n%s\n' "$([ "$bad" -eq 0 ] && echo 'all good' || echo FAILED)"
[ "$bad" -eq 0 ]
