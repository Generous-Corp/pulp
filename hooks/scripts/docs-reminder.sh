#!/bin/bash
# Remind to update docs when source files change.
# Reads TOOL_INPUT env var set by Claude Code PostToolUse hook.

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
    */core/*|*/examples/*|*/tools/cli/*)
        echo "DOCS REMINDER: Modified source file. If this changes public behavior, update docs/status/ manifests and docs/ pages."
        ;;
esac
