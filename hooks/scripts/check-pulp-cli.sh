#!/usr/bin/env bash
# check-pulp-cli.sh — Setup-hook script wired in hooks/hooks.json.
#
# Fires once per Claude Code session. Verifies the Pulp CLI is reachable
# and prints a friendly install banner if not. Always exits 0 — this
# hook is informational, never blocking. Most plugin slash commands
# (/build, /test, /create, /design, /ship, ...) shell out to `pulp`,
# so without this hook the user sees a confusing "command not found"
# the first time they invoke any command. With it they see the install
# command up-front.
#
# Output goes to stderr because Claude Code surfaces hook stderr in
# the session UI without treating it as a tool failure.

set -e

# Three states this hook handles:
#
#   1. `pulp` on PATH       → silent success (most common case after
#                              the user has run the curl|sh installer)
#   2. `pulp` not on PATH,
#       but a local source-tree build exists at ./build/pulp or
#       ./build/tools/cli/pulp-cpp → the user is a Pulp contributor
#                              working from a checkout. Tell them to
#                              symlink; do NOT push them at the install
#                              script (which would clobber their build).
#   3. nothing at all       → print the curl|sh install command.
#
# All three exit 0. Detecting state happens once, no network calls,
# no recursion into pulp itself.

# Allow tests to override the cwd / candidate paths via env so we can
# run cases 2 + 3 deterministically without polluting the user's repo.
PULP_CHECK_CWD="${PULP_CHECK_CWD:-$PWD}"

# --session-start prints the stale-CLI banner (Case 1b) on stdout, where a
# SessionStart hook's output becomes agent context. Everything else stays on
# stderr for the Setup hook's UI.
PULP_CHECK_MODE="${1:-}"

# Walk up from $1 to the Pulp source checkout (a CMakeLists.txt declaring
# `project(Pulp ...)` next to core/). Prints the root, or nothing.
pulp_checkout_root() {
    local dir="$1"
    while [ -n "$dir" ] && [ "$dir" != "/" ]; do
        if [ -d "$dir/core" ] && [ -f "$dir/CMakeLists.txt" ] &&
            grep -Eiq 'project\([[:space:]]*pulp([[:space:]]|\)|$)' "$dir/CMakeLists.txt" 2>/dev/null; then
            printf '%s\n' "$dir"
            return 0
        fi
        dir="$(dirname "$dir")"
    done
    return 0
}

# Print "MAJOR MINOR" of the first M.N.P triple in stdin, or nothing.
major_minor() {
    grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n 1 | awk -F. '{print $1, $2}'
}

# Case 1b: the `pulp` on PATH is far older than the checkout it runs in.
# Its build defaults (generator, build type, examples, parallelism) are
# baked into the binary, so it configures a current checkout the way its
# own release did. Newer CLIs refuse this themselves; this check covers the
# CLIs installed before that guard existed.
stale_cli_banner() {
    local root cli_path checkout_mm cli_mm limit
    root="$(pulp_checkout_root "$PULP_CHECK_CWD")"
    [ -n "$root" ] || return 0
    checkout_mm="$(sed -n '/project([[:space:]]*[Pp][Uu][Ll][Pp]/,/)/p' "$root/CMakeLists.txt" 2>/dev/null |
        grep -E 'VERSION[[:space:]]+[0-9]' | major_minor)"
    [ -n "$checkout_mm" ] || return 0
    cli_path="$(command -v pulp)"
    cli_mm="$(pulp version 2>/dev/null | head -n 1 | major_minor)"
    [ -n "$cli_mm" ] || return 0
    limit="${PULP_STALE_CLI_LIMIT:-50}"
    case "$limit" in ''|*[!0-9]*) limit=50 ;; esac

    local cmaj cmin pmaj pmin behind
    read -r cmaj cmin <<<"$cli_mm"
    read -r pmaj pmin <<<"$checkout_mm"
    if [ "$pmaj" -gt "$cmaj" ]; then
        behind="a newer major release"
    elif [ "$pmaj" -eq "$cmaj" ] && [ $((pmin - cmin)) -gt "$limit" ]; then
        behind="$((pmin - cmin)) releases"
    else
        return 0
    fi

    local message
    message="[pulp] STALE CLI: \`pulp\` on PATH ($cli_path) is v$cmaj.$cmin.x, $behind behind this checkout (v$pmaj.$pmin.x at $root).
It configures builds with its own old defaults (Makefiles, no build type, examples ON, a serial cmake --build), so \`pulp build\` here is slower and differently shaped than the checkout expects.
Update it:  curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
Or build this checkout's own CLI and use ./build/pulp:
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_EXAMPLES=OFF
    tools/ci/governed-build.sh cmake --build build --target pulp-rust-cli"
    if [ "$PULP_CHECK_MODE" = "--session-start" ]; then
        printf '%s\n' "$message"
    else
        printf '%s\n' "$message" >&2
    fi
}

# Case 1: pulp already on PATH.
if command -v pulp >/dev/null 2>&1; then
    stale_cli_banner || true
    exit 0
fi

# The remaining cases only concern the Setup hook's install banner.
if [ "$PULP_CHECK_MODE" = "--session-start" ]; then
    exit 0
fi

# Case 2: source-tree contributor.
SOURCE_BUILD_RUST="$PULP_CHECK_CWD/build/pulp"
SOURCE_BUILD_CPP="$PULP_CHECK_CWD/build/tools/cli/pulp-cpp"
SOURCE_BUILD=""
if [ -x "$SOURCE_BUILD_RUST" ]; then
    SOURCE_BUILD="$SOURCE_BUILD_RUST"
elif [ -x "$SOURCE_BUILD_CPP" ]; then
    SOURCE_BUILD="$SOURCE_BUILD_CPP"
fi

if [ -n "$SOURCE_BUILD" ]; then
    cat >&2 <<EOF
[pulp plugin] \`pulp\` is not on PATH, but a source-tree build exists at:
    $SOURCE_BUILD

Symlink it once and the plugin's slash commands (/build, /test, ...)
will work everywhere:

    sudo ln -s "$SOURCE_BUILD" /usr/local/bin/pulp

Or — if you'd rather use the installed binary that auto-updates:

    curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh
EOF
    exit 0
fi

# Case 3: pulp not installed at all.
cat >&2 <<EOF
[pulp plugin] \`pulp\` CLI is not installed. The plugin's slash commands
(/build, /test, /create, /design, /ship, /version, /upgrade) shell out
to it, so they will fail until you install it.

One-line install (macOS / Linux):

    curl -fsSL https://www.generouscorp.com/pulp/install.sh | sh

Then restart this Claude Code session.
EOF
exit 0
