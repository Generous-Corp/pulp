#!/usr/bin/env bash
# Decisions-contract plan-time hint — the ONE hook script BOTH agents invoke.
#
# Wired from the Claude plugin (hooks/hooks.json, PostToolUse Edit|Write) AND
# from Codex (.codex/hooks.json, PostToolUse). It surfaces the settled decisions
# relevant to a change that touches a fleet/CI config path, so agents stop
# re-proposing already-settled config (merge queue, Namespace, auto-rebase,
# bump-at-merge, depend-mode, squash, static runner names, …).
#
# Design contract (docs 21/24/25):
#   * Agent-neutral: resolves the repo root via `git rev-parse` — NOT
#     $CLAUDE_PLUGIN_ROOT — so Codex runs the identical script. All policy logic
#     lives in the neutral checker tools/scripts/decisions_contract.py.
#   * Defense-in-depth ONLY: advisory, always exits 0, never blocks. Codex
#     PreToolUse cannot hard-block anyway; the boundary is the CLI validate gate
#     + CI required checks, not this hint.
#   * No-op for external contributors: the checker's `surface` mode prints
#     nothing unless a guarded fleet/CI config path was touched, and needs no
#     Shipyard, no gh, and no network.
#
# Input: PostToolUse hooks receive the tool payload on stdin (both agents).
# We pull `file_path` out of it. If we can't, we simply do nothing.

set -u

# ── Resolve the edited file path from the tool payload (agent-neutral) ────────
# Claude exposes it as $TOOL_INPUT; Codex/others may pipe the JSON on stdin.
PAYLOAD="${TOOL_INPUT:-}"
if [ -z "$PAYLOAD" ] && [ ! -t 0 ]; then
    PAYLOAD="$(cat 2>/dev/null || true)"
fi

FILE="$(printf '%s' "$PAYLOAD" | python3 -c '
import sys, json
try:
    data = json.load(sys.stdin)
    # Claude: {"file_path": ...}; some payloads nest under tool_input.
    fp = data.get("file_path") or data.get("tool_input", {}).get("file_path", "")
    print(fp or "")
except Exception:
    pass
' 2>/dev/null)"

[ -z "$FILE" ] && exit 0

# ── Resolve the repo root via git (NOT a plugin env var) ──────────────────────
ROOT="$(git -C "$(dirname "$FILE")" rev-parse --show-toplevel 2>/dev/null)"
if [ -z "$ROOT" ]; then
    ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"
fi
[ -z "$ROOT" ] && exit 0

CHECKER="$ROOT/tools/scripts/decisions_contract.py"
CONTRACT="$ROOT/.agents/contract.toml"
[ -f "$CHECKER" ] || exit 0
[ -f "$CONTRACT" ] || exit 0

# Advisory surface for exactly this file. Prints nothing (clean no-op) unless
# the path is a guarded fleet/CI config path. Never fails the hook.
python3 "$CHECKER" --mode surface --contract "$CONTRACT" --paths "$FILE" 2>&1 || true
exit 0
