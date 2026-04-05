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
    */tools/cli/pulp_cli.cpp|*/tools/mcp/pulp_mcp.cpp)
        echo "PLUGIN SYNC: CLI or MCP server modified. Check if .agents/skills/, .claude/commands/, or hooks/ need matching updates."
        ;;
esac
