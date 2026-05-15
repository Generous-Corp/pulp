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

# Case 1: pulp already on PATH.
if command -v pulp >/dev/null 2>&1; then
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
