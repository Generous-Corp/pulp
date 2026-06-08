#!/usr/bin/env bash
# inject-claude-prefs.sh — SessionStart-hook script wired in hooks/hooks.json.
#
# Fires once per Claude Code session (matcher: startup|resume). Reads the
# `[claude] send_user_file` knob from ~/.pulp/config.toml and, when it is on
# (the DEFAULT — also the value when the file/key is absent), injects a short
# preference into the agent's context telling it to surface generated image /
# file artifacts with the SendUserFile tool so they EMBED in the Claude app
# instead of being printed as a bare path. When the knob is `off`, the hook
# emits nothing.
#
# Mechanism: SessionStart stdout is added to the model's context. We use the
# documented JSON channel (`hookSpecificOutput.additionalContext`) with
# suppressOutput so the injection is clean and not echoed to the user's UI.
# Always exits 0 — this hook is informational and must never block session init.
#
# Toggle from the CLI:
#   pulp config set claude.send_user_file off   # disable
#   pulp config set claude.send_user_file on    # re-enable (default)
#
# Tests override the config path via PULP_CONFIG_FILE so the three states
# (on / off / unset) run deterministically without touching ~/.pulp.

set -e

# Resolve the config file. PULP_CONFIG_FILE wins (tests); else $PULP_HOME or
# ~/.pulp, matching the C++ `pulp_home()` / `pulp config` layout.
if [ -n "${PULP_CONFIG_FILE:-}" ]; then
    CONFIG="$PULP_CONFIG_FILE"
elif [ -n "${PULP_HOME:-}" ]; then
    CONFIG="$PULP_HOME/config.toml"
else
    CONFIG="${HOME:-}/.pulp/config.toml"
fi

# Read [claude] send_user_file. Default "on" when the file or key is missing.
# Minimal, dependency-free TOML scan: track the current [section] header and
# return the first matching `key = "value"` (quotes/whitespace stripped) inside
# the [claude] table.
read_send_user_file() {
    local value="on"
    [ -f "$CONFIG" ] || { printf '%s' "$value"; return; }
    value=$(awk '
        /^[[:space:]]*\[/ {
            section = $0
            sub(/^[[:space:]]*\[/, "", section)
            sub(/\][[:space:]]*$/, "", section)
            gsub(/[[:space:]]/, "", section)
            next
        }
        section == "claude" {
            line = $0
            sub(/#.*/, "", line)                       # strip trailing comment
            if (line ~ /^[[:space:]]*send_user_file[[:space:]]*=/) {
                v = line
                sub(/^[^=]*=[[:space:]]*/, "", v)       # keep RHS
                gsub(/[[:space:]]/, "", v)
                gsub(/"/, "", v); gsub(/\x27/, "", v)   # strip quotes
                print tolower(v)
                exit
            }
        }
    ' "$CONFIG")
    [ -n "$value" ] || value="on"
    printf '%s' "$value"
}

STATE="$(read_send_user_file)"

# Off → no injection. Anything other than an explicit "off" (on / unset /
# unrecognized) defaults to the enabled behavior, since the feature defaults on.
if [ "$STATE" = "off" ]; then
    exit 0
fi

cat <<'JSON'
{"hookSpecificOutput":{"hookEventName":"SessionStart","additionalContext":"Pulp plugin preference (claude.send_user_file = on): when you produce an image or file artifact for the user — screenshots, rendered designs, generated diagrams, build outputs, reports — surface it with the SendUserFile tool so it embeds in the Claude app, rather than only printing its path. Skip this for paths the user already has open or for plain text. The user can disable this with: pulp config set claude.send_user_file off"},"suppressOutput":true}
JSON
exit 0
