#!/usr/bin/env bash
# install-githooks.sh — point this checkout's git at .githooks/.
#
# Idempotent. Safe to run repeatedly. Called by setup.sh during bootstrap
# and available standalone when developers want to enable the hooks in a
# pre-existing checkout.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
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
