#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LABELS="self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro,pulp-auto-linux-x64"
export PULP_RUNNER_LABELS="$LABELS"
export PULP_RUNNER_NAME_PREFIX="pulp-auto-ephemeral"
export PULP_LINUX_RUNNER_GROUP_ID="${PULP_TRUSTED_LINUX_RUNNER_GROUP_ID:-3}"
export PULP_LINUX_RUNNER_GROUP_PROFILE="trusted"
exec "$SCRIPT_DIR/proxmox-ephemeral-runner-linux.sh" "$@"
