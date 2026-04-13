#!/usr/bin/env bash
# install-githooks.sh — point this checkout's git at .githooks/.
#
# Idempotent. Safe to run repeatedly. Called by setup.sh during bootstrap
# and available standalone when developers want to enable the hooks in a
# pre-existing checkout.

set -euo pipefail

# Walk up from this script's directory until we find a .git entry. Works
# whether the script lives at tools/scripts/ (pulp) or scripts/ (Shipyard)
# without needing a layout-specific relative path.
script_dir="$(cd "$(dirname "$0")" && pwd)"
candidate="$script_dir"
ROOT=""
while [ "$candidate" != "/" ] && [ -n "$candidate" ]; do
    if [ -e "$candidate/.git" ]; then
        ROOT="$candidate"
        break
    fi
    candidate="$(dirname "$candidate")"
done
if [ -z "$ROOT" ]; then
    echo "install-githooks: could not find git repo root above $script_dir" >&2
    exit 1
fi
cd "$ROOT"

if [ ! -d ".githooks" ]; then
    echo "install-githooks: .githooks/ not found at $ROOT" >&2
    exit 1
fi

current="$(git config --get core.hooksPath || true)"
if [ "$current" = ".githooks" ]; then
    echo "install-githooks: already configured (core.hooksPath=.githooks)."
    exit 0
fi

git config core.hooksPath .githooks
chmod +x .githooks/* 2>/dev/null || true
echo "install-githooks: set core.hooksPath=.githooks"
