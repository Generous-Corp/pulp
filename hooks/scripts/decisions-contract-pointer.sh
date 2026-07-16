#!/usr/bin/env bash
# Decisions-contract SessionStart pointer — agent-neutral, zero-cost.
#
# Codex reads AGENTS.md/CLAUDE.md once per session and mid-session edits do not
# refresh, so a SessionStart pointer keeps the contract's location in context.
# Advisory only: it prints a one-line pointer and exits 0. It never blocks and
# is a no-op in any checkout that lacks the contract file.
#
# Resolves the repo root via git — NOT $CLAUDE_PLUGIN_ROOT — so Claude and Codex
# run the identical script.

set -u

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"
[ -z "$ROOT" ] && exit 0
[ -f "$ROOT/.agents/contract.toml" ] || exit 0
[ -f "$ROOT/tools/scripts/decisions_contract.py" ] || exit 0

cat <<'EOF'
── decisions contract ──
Settled build-system / CI / release-automation decisions live in
`.agents/contract.toml` (layered: default + pulp overlay). Before proposing a
change to fleet/CI config, read the relevant rows:
    python3 tools/scripts/decisions_contract.py --mode list        # all rows
    python3 tools/scripts/decisions_contract.py --mode surface --base origin/main
Reversing a decision requires proving its incident class can no longer occur.
EOF
exit 0
