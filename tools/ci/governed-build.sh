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
# or a plain checkout) it falls back to the tier-0 bound. Usage:
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

# --- Tier-0 bound: min(cores, RAM_budget / 1.5 GiB), always >= 1 ---------------
# This is the no-tartci bound: it keeps a build from exhausting RAM, but on a
# big-RAM host the memory axis never binds and it degrades to the full core
# count. It is therefore NOT a saturation bound and must never be used as the
# response to a lease denial — a denial means cores are already spoken for, and
# tier-0 knows nothing about that. See denial handling below.
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

# Conservative floor for a leaseless build on a host whose lease store said no.
# Small enough to make progress without meaningfully adding to the load that
# caused the denial.
min_jobs() {
  local floor="${PULP_GOVERNED_BUILD_MIN_JOBS:-2}"
  [ "$floor" -ge 1 ] 2>/dev/null || floor=2
  echo "$floor"
}

# Cores tartci will actually grant a non-gate lease right now, or "" if unknown.
# Matches the key anywhere in the payload so both pretty-printed and compact
# --json output parse.
# Never fails: an unreadable store or an older schema yields "" (unknown), which
# the caller treats as no-capacity. `|| true` keeps `set -e` from killing the
# build over a lease-store hiccup.
available_cores() {
  local status avail
  status="$("$TARTCI_BIN" leases status --json 2>/dev/null)" || return 0
  avail="$(printf '%s' "$status" \
    | grep -o '"non_gate_available_cores"[[:space:]]*:[[:space:]]*[0-9][0-9]*' \
    | grep -o '[0-9][0-9]*$' | head -n 1 || true)"
  if [ -n "$avail" ]; then echo "$avail"; fi
  return 0
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

# Acquire a build-priority lease of $1 cores. Omit --mem-mb so tartci derives
# the build's memory from cores*per-job (the right estimate for a compile set).
acquire_lease() {
  "$TARTCI_BIN" leases acquire \
    --id "$LEASE_ID" --cores "$1" --priority build \
    --kind shipyard-local --owner "governed-build" --pid "$$" \
    --job-id "${GITHUB_RUN_ID:-}" --json >/dev/null 2>&1
}

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
  if acquire_lease "$jobs"; then
    log "lease acquired id=$LEASE_ID cores=$jobs (host profile)"
  else
    # Denied: the host cannot spare the profile-sized lease right now. Back off
    # to what tartci says is actually free and ask again for that — a denial is
    # a report of real contention, so the retry must be sized from the store's
    # own capacity, never from this host's core count.
    avail="$(available_cores)"
    if [ -n "$avail" ] && [ "$avail" -ge 1 ] 2>/dev/null; then
      if [ "$avail" -lt "$jobs" ]; then jobs="$avail"; fi
      if acquire_lease "$jobs"; then
        log "lease denied at profile size — acquired id=$LEASE_ID cores=$jobs (available capacity)"
      else
        # Lost a race for the remaining cores.
        LEASE_ID=""
        jobs="$(min_jobs)"
        log "lease denied at available capacity — proceeding leaseless at -j$jobs (floor)"
      fi
    else
      # Zero free cores, or the store did not report capacity. Either way this
      # host is not offering any, so take the floor and nothing more.
      LEASE_ID=""
      jobs="$(min_jobs)"
      log "lease denied, no capacity reported — proceeding leaseless at -j$jobs (floor)"
    fi
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
