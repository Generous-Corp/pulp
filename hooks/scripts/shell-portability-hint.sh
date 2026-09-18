#!/usr/bin/env bash
# Advisory shell portability hint for agent Bash/exec commands.
#
# The tracked-file checker cannot see commands an agent types directly. This
# hook extracts the command from the agent-neutral tool payload, checks it as
# zsh by default, and prints a precise warning while always exiting zero.
set -u

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
[ -n "${ROOT}" ] || exit 0
CHECKER="${ROOT}/tools/scripts/shell_portability_check.py"
[ -f "${CHECKER}" ] || exit 0

PYTHON=""
for candidate in python3 python3.14 python3.13 python3.12 python3.11 \
                /opt/homebrew/bin/python3 /usr/local/bin/python3; do
    resolved="$(command -v "${candidate}" 2>/dev/null || true)"
    [ -n "${resolved}" ] || continue
    PYTHON="${resolved}"
    break
done
[ -n "${PYTHON}" ] || exit 0

PAYLOAD="${TOOL_INPUT:-}"
if [ -z "${PAYLOAD}" ] && [ ! -t 0 ]; then
    PAYLOAD="$(cat 2>/dev/null || true)"
fi

COMMAND="$(printf '%s' "${PAYLOAD}" | "${PYTHON}" -c '
import json, sys
try:
    data = json.load(sys.stdin)
    tool_input = data.get("tool_input", {})
    value = (data.get("command") or data.get("cmd") or
             tool_input.get("command") or tool_input.get("cmd") or "")
    print(value)
except Exception:
    pass
' 2>/dev/null)"
[ -n "${COMMAND}" ] || exit 0

SHELL_NAME="${PULP_AGENT_SHELL:-${SHELL##*/}}"
case "${SHELL_NAME}" in
    zsh|bash) ;;
    *) exit 0 ;;
esac
"${PYTHON}" "${CHECKER}" --command "${COMMAND}" --shell "${SHELL_NAME}" 2>&1 || true
exit 0
