#!/usr/bin/env bash
# cli_version_check.sh — plugin ↔ CLI skew detection helper.
#
# Sourced by Pulp Claude-Code plugin skills that shell out to `pulp`.
# On the first invocation in a session, compares the installed CLI's
# version against the plugin's declared `min_cli_version` (from
# `plugin.json`, via `pulp doctor --versions --json`) and prints a
# one-line upgrade hint on skew (#551).
#
# Plugin location comes from `pulp doctor --versions --json` (#546) — never from
# an env var, a Claude-Code-specific lookup, or a self-locating trick.
# That keeps the helper portable to any shell context and avoids
# coupling Pulp skills to Claude-Code internals.
#
# The helper lives in `tools/scripts/` because it's a shared shell
# utility, not a skill of its own — putting it under `.agents/skills/`
# would require a stub SKILL.md and a skill_path_map entry. Keeping it
# here also mirrors `install.sh`, `cli_sync_check.py`, and the other
# cross-cutting shell helpers.
#
# Usage (sourced):
#   source "$PULP_REPO_ROOT/tools/scripts/cli_version_check.sh"
#   pulp_cli_version_check   # no-ops silently if already checked this session
#
# Usage (direct probe):
#   bash tools/scripts/cli_version_check.sh
#
# Behaviour contract (stable — test/test_cli_skew_banner.cpp depends on it):
#   - Exits 0 always (advisory only — never block a skill).
#   - Writes the banner to stderr (stdout is reserved for data).
#   - Honours $PULP_SKEW_CHECK_CACHE to override the session marker dir
#     (test harness uses this to simulate fresh sessions).
#   - Honours $PULP_SKEW_CHECK_DISABLE=1 to turn the check off entirely
#     (users who want silence without an `update.mode = off` round-trip).
#   - Emits the banner at most once per session (keyed by PPID).

set -u

pulp_cli_version_check() {
    # Explicit opt-out.
    if [ "${PULP_SKEW_CHECK_DISABLE:-0}" = "1" ]; then
        return 0
    fi

    # `pulp` must be on PATH. If it isn't, that's a separate problem
    # (the skill will surface its own "pulp not found" message) and
    # not our concern here. We stay silent — a missing CLI is a user
    # state, not a skew.
    if ! command -v pulp >/dev/null 2>&1; then
        return 0
    fi

    # Session marker. PPID is the parent shell — stable within a single
    # bash driver script across multiple calls. Test harnesses override
    # the cache dir so a fresh session can be simulated.
    local cache_dir="${PULP_SKEW_CHECK_CACHE:-${TMPDIR:-/tmp}}"
    mkdir -p "$cache_dir" 2>/dev/null || return 0
    local marker="$cache_dir/pulp-skew-check-${PPID:-0}"
    if [ -f "$marker" ]; then
        return 0
    fi

    # Grab the diagnostic once. --json is the stable-shape surface
    # (#546) — do NOT parse human output. If the CLI is so
    # old it lacks --json or --versions, the invocation fails and we
    # silently stop (older users will upgrade for other reasons).
    local json
    json="$(pulp doctor --versions --json 2>/dev/null)" || {
        : >"$marker" 2>/dev/null || true
        return 0
    }
    if [ -z "$json" ]; then
        : >"$marker" 2>/dev/null || true
        return 0
    fi

    # Extract fields with POSIX awk. We deliberately avoid jq — this
    # helper must work on every machine a user might run Claude Code
    # on, including slim Git-Bash shells on Windows and ops containers
    # without jq pre-installed.
    local cli_raw plugin_min_raw
    cli_raw="$(printf '%s' "$json" | \
        awk 'BEGIN{RS="\"cli\"[ \t]*:"}NR==2{print}' | \
        awk 'BEGIN{RS="\"raw\"[ \t]*:[ \t]*\""}NR==2{print}' | \
        awk -F'"' 'NR==1{print $1}')"
    plugin_min_raw="$(printf '%s' "$json" | \
        awk 'BEGIN{RS="\"plugin_min_cli\"[ \t]*:"}NR==2{print}' | \
        awk 'BEGIN{RS="\"raw\"[ \t]*:[ \t]*\""}NR==2{print}' | \
        awk -F'"' 'NR==1{print $1}')"

    # Missing plugin_min_cli (older plugin builds) —
    # silently skip the check but still mark the session so we don't
    # re-probe on every skill load in that session.
    if [ -z "$plugin_min_raw" ] || [ -z "$cli_raw" ]; then
        : >"$marker" 2>/dev/null || true
        return 0
    fi

    # Compare semvers. Anything non-numeric (dev builds, tags) → skip.
    _pulp_skew_parse() {
        # $1 = raw string; prints "MAJ MIN PAT" on stdout, exit 1 if
        # the string isn't a clean numeric triple.
        local v="${1#v}"
        v="${v#V}"
        case "$v" in
            *[!0-9.]*) return 1 ;;
            *.*.*)     ;;
            *)         return 1 ;;
        esac
        local maj="${v%%.*}"
        local rest="${v#*.}"
        local min="${rest%%.*}"
        local pat="${rest#*.}"
        case "$maj$min$pat" in
            *[!0-9]*) return 1 ;;
        esac
        printf '%s %s %s\n' "$maj" "$min" "$pat"
    }

    local cli_parts min_parts
    cli_parts="$(_pulp_skew_parse "$cli_raw")" || {
        : >"$marker" 2>/dev/null || true
        return 0
    }
    min_parts="$(_pulp_skew_parse "$plugin_min_raw")" || {
        : >"$marker" 2>/dev/null || true
        return 0
    }

    # shellcheck disable=SC2086
    set -- $cli_parts; local cli_maj=$1 cli_min=$2 cli_pat=$3
    # shellcheck disable=SC2086
    set -- $min_parts; local m_maj=$1 m_min=$2 m_pat=$3

    local skew=0
    if [ "$m_maj" -gt "$cli_maj" ]; then
        skew=1
    elif [ "$m_maj" -eq "$cli_maj" ] && [ "$m_min" -gt "$cli_min" ]; then
        skew=1
    elif [ "$m_maj" -eq "$cli_maj" ] && [ "$m_min" -eq "$cli_min" ] && \
         [ "$m_pat" -gt "$cli_pat" ]; then
        skew=1
    fi

    if [ "$skew" = "1" ]; then
        # Stable banner copy — mirrored in:
        #   - .agents/skills/upgrade/SKILL.md (references this helper)
        #   - .agents/skills/cli-maintenance/SKILL.md
        #   - docs/guides/versioning.md
        # If you change this text, update test/test_cli_skew_banner.cpp
        # ("Stable banner copy") in the same PR.
        printf '[pulp] Claude plugin requires CLI >= v%s but installed CLI is v%s. Run `pulp upgrade` or `/upgrade` in Claude Code.\n' \
            "$plugin_min_raw" "$cli_raw" >&2
    fi

    : >"$marker" 2>/dev/null || true
    return 0
}

# Direct invocation (e.g. `bash cli_version_check.sh`) runs the check.
# When sourced, callers invoke `pulp_cli_version_check` explicitly so
# the trigger point is visible at every call site.
if [ "${BASH_SOURCE[0]:-$0}" = "$0" ]; then
    pulp_cli_version_check
fi
