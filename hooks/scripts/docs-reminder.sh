#!/bin/bash
# Remind to update docs when source files change.
# Claude supplies TOOL_INPUT; Codex supplies the hook envelope on stdin.

PAYLOAD="${TOOL_INPUT:-}"
if [ -z "$PAYLOAD" ] && [ ! -t 0 ]; then
    PAYLOAD="$(cat 2>/dev/null || true)"
fi

FILE=$(printf '%s' "$PAYLOAD" | python3 -c "
import sys, json
try:
    data = json.load(sys.stdin)
    tool_input = data.get('tool_input', data)
    print(tool_input.get('file_path', '') if isinstance(tool_input, dict) else '')
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
    */.github/workflows/*|*/tools/local-ci/*|*/tools/scripts/*)
        echo "CI REMINDER: Modified CI/infrastructure file. Update docs/guides/local-ci.md, CLAUDE.md CI Workflow section, and README if the merge/validation process changed."
        ;;
esac
