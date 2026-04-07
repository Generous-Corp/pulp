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
