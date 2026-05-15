#!/bin/bash
# Alert when CLI commands change so the plugin stays in sync.
# Runs as a PostToolUse hook on Edit|Write operations.

FILE=$(echo "$TOOL_INPUT" | python3 -c "
import sys, json
try:
    data = json.load(sys.stdin)
    print(data.get('file_path', ''))
except:
    pass
" 2>/dev/null)

if [ -z "$FILE" ]; then
    exit 0
fi

case "$FILE" in
    */tools/cli/pulp_cli.cpp|*/tools/cli/cmd_*.cpp|*/tools/cli/cli_common.hpp)
        echo "CLI SYNC: CLI source modified. Run: python3 tools/scripts/cli_sync_check.py"
        echo "  Update: cli-commands.yaml, slash commands, reference docs, skills"
        ;;
    */tools/mcp/pulp_mcp.cpp)
        echo "CLI SYNC: MCP server modified. Verify MCP tools still map to CLI commands."
        echo "  Run: python3 tools/scripts/check_cli_mcp_parity.py --mode=report"
        echo "  If you intentionally diverged, update tools/scripts/cli_mcp_parity_baseline.json."
        ;;
    */tools/scripts/cli_mcp_parity_baseline.json)
        echo "CLI SYNC: parity baseline modified. Verify the change is intentional."
        echo "  Run: python3 tools/scripts/check_cli_mcp_parity.py --mode=report"
        ;;
    */.claude/commands/*.md)
        echo "CLI SYNC: Slash command modified. Verify it matches cli-commands.yaml and CLI source."
        ;;
    */.agents/skills/*/SKILL.md)
        echo "CLI SYNC: Skill modified. Verify CLI command references are current."
        ;;
    */docs/status/cli-commands.yaml)
        echo "CLI SYNC: CLI manifest modified. Verify it matches CLI source and slash commands."
        ;;
esac

# ── Layer-1 versioning & skill-sync hints ────────────────────────────────
# Advisory only. The authoritative gate is CI + .githooks/pre-push; this
# just surfaces drift as early as possible so pulp pr at push time rarely
# hard-fails. See docs/guides/versioning.md for the full three-layer design.
#
# Locate the repo root relative to $FILE so agents in multi-worktree setups
# call the right scripts.
REPO_ROOT=""
candidate="$(dirname "$FILE")"
while [ -n "$candidate" ] && [ "$candidate" != "/" ]; do
    if [ -f "$candidate/tools/scripts/versioning.json" ]; then
        REPO_ROOT="$candidate"
        break
    fi
    candidate="$(dirname "$candidate")"
done

if [ -n "$REPO_ROOT" ]; then
    VBC="$REPO_ROOT/tools/scripts/version_bump_check.py"
    SSC="$REPO_ROOT/tools/scripts/skill_sync_check.py"
    CSC="$REPO_ROOT/tools/scripts/compat_sync_check.py"
    if [ -x "$VBC" ]; then
        "$VBC" --base origin/main --config "$REPO_ROOT/tools/scripts/versioning.json" --mode=hint 2>/dev/null || true
    fi
    if [ -x "$SSC" ]; then
        "$SSC" --base origin/main --config "$REPO_ROOT/tools/scripts/versioning.json" --mode=hint 2>/dev/null || true
    fi
    # Compat-sync hint (#1029). Runs against tools/scripts/compat_path_map.json
    # by default; advisory only — same shape as the version/skill hints.
    if [ -x "$CSC" ]; then
        "$CSC" --base origin/main --mode=hint 2>/dev/null || true
    fi

    # CLI ↔ MCP parity hint (pulp #1997). Advisory only — surfaces drift
    # while iterating in agent loops so the authoritative gate in
    # .github/workflows/version-skill-check.yml rarely hard-fails on push.
    # Anything new in cli_only or mcp_only baseline is intentional;
    # anything missing from BOTH is a fresh gap that should be either
    # promoted to MCP or recorded in the baseline with a one-line reason.
    PARITY="$REPO_ROOT/tools/scripts/check_cli_mcp_parity.py"
    if [ -x "$PARITY" ]; then
        "$PARITY" --mode=hint --no-color 2>/dev/null || true
    fi
fi
