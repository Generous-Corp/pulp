#!/usr/bin/env bash
# Run a build command under a tartci host build-lease when one is available.
#
# Shipyard's `local` mac backend runs the .shipyard/config.toml build string
# directly on the host — it does NOT go through the pulp CLI, so the CLI's
# lease integration never sees it. That is the path that oversubscribed a
# shared Mac (a validation build racing agent builds with no shared budget).
#
# This wrapper closes that gap: when tartci is installed it acquires a build
# lease sized from the host profile, exports the granted parallelism, runs the
# build, and releases on exit. When tartci is absent (e.g. inside a build VM,
# or a plain checkout) it falls back to a bounded local parallelism so the
# build is never unbounded. Usage:
#
#   tools/ci/governed-build.sh cmake --build build [--target ...]
#
# The build command MUST NOT carry its own --parallel/-j (a bare flag is
# rejected by build_parallelism_guard.py anyway); CMAKE_BUILD_PARALLEL_LEVEL
# from this wrapper governs it.
set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "governed-build: usage: governed-build.sh <build command...>" >&2
  exit 2
fi

log() { echo "[governed-build] $*" >&2; }

# --- Tier-0 fallback bound: min(cores, RAM_budget / 1.5 GiB), always >= 1 ------
tier0_jobs() {
  local cores mem_kb mem_mb mem_jobs
  cores="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
  [ "$cores" -ge 1 ] 2>/dev/null || cores=4
  # Physical RAM in MB (macOS + Linux); 0 → unknown → core-bound only.
  mem_mb=0
  if mem_bytes="$(sysctl -n hw.memsize 2>/dev/null)"; then
    mem_mb=$(( mem_bytes / 1024 / 1024 ))
  elif mem_kb="$(awk '/MemTotal/ {print $2; exit}' /proc/meminfo 2>/dev/null)"; then
    mem_mb=$(( mem_kb / 1024 ))
  fi
  local jobs="$cores"
  if [ "$mem_mb" -gt 0 ]; then
    # ~1.5 GiB per compile job, reserve ~25% for the OS/window server.
    mem_jobs=$(( mem_mb * 3 / 4 / 1536 ))
    [ "$mem_jobs" -lt 1 ] && mem_jobs=1
    [ "$mem_jobs" -lt "$jobs" ] && jobs="$mem_jobs"
  fi
  [ "$jobs" -ge 1 ] 2>/dev/null || jobs=1
  echo "$jobs"
}

find_tartci() {
  if [ -n "${PULP_TARTCI_BIN:-}" ] && [ -x "${PULP_TARTCI_BIN}" ]; then
    echo "${PULP_TARTCI_BIN}"; return 0
  fi
  command -v tartci 2>/dev/null || true
}

LEASE_ID=""
TARTCI_BIN=""

release_lease() {
  if [ -n "$LEASE_ID" ] && [ -n "$TARTCI_BIN" ]; then
    "$TARTCI_BIN" leases release --id "$LEASE_ID" --json >/dev/null 2>&1 || true
    LEASE_ID=""
  fi
}
trap release_lease EXIT INT TERM

jobs=""
qos=""

if [ "${PULP_TARTCI_LEASES:-}" != "0" ]; then
  TARTCI_BIN="$(find_tartci)"
fi

if [ -n "$TARTCI_BIN" ] && profile="$("$TARTCI_BIN" host-profile 2>/dev/null)"; then
  # Host profile is available → size the lease from it.
  jobs="$(printf '%s\n' "$profile" | awk -F= '/^PULP_BUILD_JOBS=/{print $2; exit}')"
  qos="$(printf '%s\n' "$profile" | awk -F= '/^TARTCI_AGENT_QOS=/{print $2; exit}')"
  [ -n "$jobs" ] && [ "$jobs" -ge 1 ] 2>/dev/null || jobs="$(tier0_jobs)"
  LEASE_ID="pulp-shipyard-local-$$-$(date +%s 2>/dev/null || echo 0)"
  # Acquire a build-priority lease. Omit --mem-mb so tartci derives the build's
  # memory from cores*per-job (the right estimate for a compile job set).
  if "$TARTCI_BIN" leases acquire \
        --id "$LEASE_ID" --cores "$jobs" --priority build \
        --kind shipyard-local --owner "governed-build" --pid "$$" \
        --job-id "${GITHUB_RUN_ID:-}" --json >/dev/null 2>&1; then
    log "lease acquired id=$LEASE_ID cores=$jobs (host profile)"
  else
    # Host is saturated (or the store is unhappy). Do NOT fail the build and do
    # NOT pile on: drop to the conservative tier-0 bound and proceed leaseless.
    LEASE_ID=""
    jobs="$(tier0_jobs)"
    log "lease denied/unavailable — proceeding leaseless at -j$jobs"
  fi
else
  # No tartci (build VM / plain checkout): bounded tier-0 parallelism.
  jobs="$(tier0_jobs)"
  log "no tartci host profile — bounded local build at -j$jobs"
fi

export CMAKE_BUILD_PARALLEL_LEVEL="$jobs"
export CTEST_PARALLEL_LEVEL="$jobs"

# Run the build as a CHILD (not exec) so the EXIT trap fires and the lease is
# released even on failure. Background QoS on laptop-class hosts keeps a shared
# machine's UI responsive (taskpolicy only re-prioritizes; the -j cap above is
# the real bound).
rc=0
if [ "$qos" = "background" ] && command -v taskpolicy >/dev/null 2>&1 \
    && [ "${PULP_TARTCI_TASKPOLICY:-}" != "0" ]; then
  taskpolicy -b "$@" || rc=$?
else
  "$@" || rc=$?
fi
exit "$rc"
