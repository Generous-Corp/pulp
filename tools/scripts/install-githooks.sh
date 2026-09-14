#!/usr/bin/env bash
# install-githooks.sh — wire this checkout's local git config to the repo's
# own hooks and merge drivers.
#
# Two jobs, because both are local-config state that a clone cannot carry:
#   1. core.hooksPath -> .githooks/
#   2. the pulp-gpu-ledger merge driver named by .gitattributes
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
else
    git config core.hooksPath .githooks
    chmod +x .githooks/* 2>/dev/null || true
    echo "install-githooks: set core.hooksPath=.githooks"
fi

# .gitattributes routes the generated GPU handoff ledger and its receipt to
# merge=pulp-gpu-ledger. Git resolves that name against local config, and a
# name with no driver behind it silently falls back to the ordinary text merge
# — which is the conflict treadmill the driver exists to end. Register it here
# so the same bootstrap that installs the hooks installs the driver.
driver="tools/scripts/gpu_ledger_merge_driver.sh"
if [ ! -x "$driver" ]; then
    echo "install-githooks: $driver missing or not executable at $ROOT" >&2
    exit 1
fi
git config merge.pulp-gpu-ledger.name \
    "GPU handoff ledger: resolve to a sentinel that must be regenerated"
git config merge.pulp-gpu-ledger.driver "$driver %A %B"
echo "install-githooks: registered merge driver pulp-gpu-ledger"
