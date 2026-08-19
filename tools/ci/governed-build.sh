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

# Print who currently holds the host's cores, and whether each holder's process
# is still alive.
#
# A denial and a lie look identical from here: both produce a slow build and one
# log line. They are not the same event. A denial is the governor working — the
# host really is busy. A lie is a lease left behind by a VM or build that died
# uncleanly, and nothing reconciles the store against reality, so those cores
# stay spoken for indefinitely. On 2026-08-16 two dead VMs held 12 of 14 cores
# and pinned a critical-path build to the floor for its whole run; the store read
# 0/14 minutes later.
#
# `alive=no` is the tell, and it is the reason this prints the owning pid rather
# than only the core counts: a holder whose process is gone is a stale lease, and
# the correct response is to reap and restart rather than to wait. An incremental
# `cmake --build` loses no objects across a restart.
#
# Diagnostics only: never fails, never changes the build's parallelism or its
# exit status.
log_lease_holders() {
  local status
  status="$("$TARTCI_BIN" leases status --json 2>/dev/null)" || return 0
  [ -n "$status" ] || return 0

  if command -v python3 >/dev/null 2>&1; then
    printf '%s' "$status" | python3 -c '
import json, os, sys

try:
    payload = json.load(sys.stdin)
except Exception:
    sys.exit(0)

leases = payload.get("leases") or []
capacity = payload.get("capacity") or {}
print("[governed-build] denial detail: used_cores=%s/%s non_gate_available=%s holders=%d"
      % (capacity.get("used_cores", "?"), capacity.get("total_cores", "?"),
         capacity.get("non_gate_available_cores", "?"), len(leases)))
stale = 0
for lease in leases:
    pid = lease.get("pid")
    alive = "unknown"
    if isinstance(pid, int) and pid > 0:
        try:
            os.kill(pid, 0)
            alive = "yes"
        except ProcessLookupError:
            alive = "no"
            stale += 1
        except PermissionError:
            alive = "yes"
    print("[governed-build]   holder id=%s kind=%s cores=%s owner=%s pid=%s alive=%s created=%s"
          % (lease.get("id", "?"), lease.get("command_kind", "?"),
             lease.get("lease_size_cores", "?"), lease.get("owner", "?"),
             pid, alive, lease.get("created_at", "?")))
if stale:
    print("[governed-build] %d holder(s) reference a process that no longer exists — "
          "this denial is likely stale, not real contention. Reap the store "
          "(`tartci leases reap`) and restart the build to re-acquire." % stale)
' >&2 || true
  else
    # No python3: dump whatever the human-readable view says. Less precise (no
    # liveness), but still names the holders instead of leaving the operator
    # with a bare "denied".
    "$TARTCI_BIN" leases status 2>/dev/null | sed 's/^/[governed-build]   /' >&2 || true
  fi
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
HEARTBEAT_PID=""

# --- live-build marker -------------------------------------------------------
#
# Shipyard's `local` mac backend builds IN THE CHECKOUT, and nothing tells you a
# validation run is using the tree you are about to edit. On 2026-08-16 an agent
# merged twice into a worktree while a validation build was live inside it; the
# run reached 11% in two hours and died on the lane's 7200s cap, reporting
# `Validation timed out` — which names the target and not the cause. Every piece
# of information needed was available and nothing prompted anyone to look.
#
# This marker is NOT the interrupted-build sentinel (`build-dir-sentinel.sh`) and
# cannot be folded into it. That one lives in the BUILD dir and answers "was this
# left mid-build", deliberately surviving SIGKILL — so it reads identically during
# a live build and after a killed one. That ambiguity is its design. This one
# answers the opposite question, "is a build running RIGHT NOW", and resolves the
# ambiguity by recording the pid: a reader probes it with kill(pid, 0), exactly as
# the lease-holder logging above does, so a marker left behind by a killed build
# is distinguishable from a live one rather than being indistinguishable by
# construction.
#
# It lives at the SOURCE-tree root, not the build dir, because the hazard is
# mutating the sources — a build dir can be wiped, a half-merged tree under a
# running CMake cannot be un-mutated.
BUILD_MARKER=""

write_build_marker() {
  local root
  root="$(git rev-parse --show-toplevel 2>/dev/null)" || return 0
  [ -n "$root" ] || return 0
  BUILD_MARKER="$root/.pulp-build-active"
  # Best-effort: an unwritable tree must never fail the build.
  {
    echo "pid=$$"
    echo "started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)"
    echo "started_epoch=$(date +%s 2>/dev/null || echo 0)"
    echo "jobs=${jobs:-unknown}"
    echo "lease=${LEASE_ID:-none}"
    echo "run_id=${GITHUB_RUN_ID:-}"
    echo "command=$*"
  } >"$BUILD_MARKER" 2>/dev/null || BUILD_MARKER=""
}

remove_build_marker() {
  [ -n "$BUILD_MARKER" ] && rm -f "$BUILD_MARKER" 2>/dev/null
  BUILD_MARKER=""
  return 0
}


# --- Lease heartbeat ----------------------------------------------------------
# tartci stamps a heartbeat when a lease is acquired and marks the lease stale
# once that stamp ages past the store's threshold (TARTCI_LEASE_STALE_SECS,
# default 300s). A compile set runs far longer than that, so acquisition's
# single stamp is not enough: `tartci status` reported
# `stale_heartbeat_live_owner` for a build whose owner PID and whole `cmake
# --build` descendant tree were alive and healthy (M5, 2026-08-15). Stale
# telemetry on a live owner is worse than none — it invites a controller or an
# operator to preempt a working build, or to size the host as if those cores
# were free.
#
# The refresh interval is deliberately a fraction of the threshold so two
# missed beats still do not read as stale.
HEARTBEAT_INTERVAL="${PULP_GOVERNED_BUILD_HEARTBEAT_SECS:-60}"

start_heartbeat() {
  # Only a lease this process actually holds may be refreshed. A leaseless
  # fallback has nothing to keep alive, and refreshing someone else's id would
  # hide a genuinely dead owner.
  [ -n "$LEASE_ID" ] && [ -n "$TARTCI_BIN" ] || return 0
  [ "$HEARTBEAT_INTERVAL" -ge 1 ] 2>/dev/null || return 0
  local parent=$$
  (
    # `sleep` runs in the BACKGROUND and is waited on, rather than being the
    # foreground child. bash defers a trapped signal until the current
    # foreground command returns, so a plain `while sleep N` refresher ignores
    # its own SIGTERM until the interval elapses — which made teardown of every
    # governed build block for up to one full interval. `wait` is
    # signal-interruptible, so this exits promptly.
    trap 'exit 0' TERM INT
    while :; do
      sleep "$HEARTBEAT_INTERVAL" &
      wait $! 2>/dev/null || exit 0
      # Watch the parent explicitly. SIGKILL skips the EXIT trap, so without
      # this the refresher would outlive the build and keep a lease looking
      # live forever — the exact inverse of the bug being fixed, and a harder
      # one to see because the telemetry would look correct.
      kill -0 "$parent" 2>/dev/null || exit 0
      "$TARTCI_BIN" leases heartbeat --id "$LEASE_ID" --json >/dev/null 2>&1 || true
    done
  ) >/dev/null 2>&1 &
  # The redirection above is load-bearing, not tidiness. A background child
  # inherits the caller's stdout, and a caller that runs this wrapper inside a
  # command substitution blocks until EVERY holder of that pipe closes it — so
  # an un-redirected refresher makes `out="$(governed-build.sh ...)"` hang for
  # a full heartbeat interval after the build already finished.
  HEARTBEAT_PID=$!
  log "heartbeat every ${HEARTBEAT_INTERVAL}s for lease id=$LEASE_ID"
}

stop_heartbeat() {
  if [ -n "$HEARTBEAT_PID" ]; then
    kill "$HEARTBEAT_PID" 2>/dev/null || true
    wait "$HEARTBEAT_PID" 2>/dev/null || true
    HEARTBEAT_PID=""
  fi
}

# One cleanup for both, and the ORDER is load-bearing.
#
# stop_heartbeat runs first so the refresher cannot re-stamp a lease id that
# has already been handed back. remove_build_marker runs before the release
# for the same reason in the other direction: the marker advertises a live
# build in this checkout, and it must not outlive the build it describes.
cleanup() {
  stop_heartbeat
  remove_build_marker
  if [ -n "$LEASE_ID" ] && [ -n "$TARTCI_BIN" ]; then
    "$TARTCI_BIN" leases release --id "$LEASE_ID" --json >/dev/null 2>&1 || true
    LEASE_ID=""
  fi
}
trap cleanup EXIT INT TERM

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
    start_heartbeat
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
        start_heartbeat
      else
        # Lost a race for the remaining cores.
        LEASE_ID=""
        jobs="$(min_jobs)"
        log "lease denied at available capacity — proceeding leaseless at -j$jobs (floor)"
        log_lease_holders
      fi
    else
      # Zero free cores, or the store did not report capacity. Either way this
      # host is not offering any, so take the floor and nothing more.
      LEASE_ID=""
      jobs="$(min_jobs)"
      log "lease denied, no capacity reported — proceeding leaseless at -j$jobs (floor)"
      log_lease_holders
    fi
  fi
else
  # No tartci (build VM / plain checkout): bounded tier-0 parallelism.
  jobs="$(tier0_jobs)"
  log "no tartci host profile — bounded local build at -j$jobs"
fi

export CMAKE_BUILD_PARALLEL_LEVEL="$jobs"
export CTEST_PARALLEL_LEVEL="$jobs"

# Name an unresolvable build command before running it.
#
# A Shipyard dispatch runs in a NON-INTERACTIVE login shell, which does not read
# the interactive profile that puts /opt/homebrew/bin on PATH. `cmake` then does
# not resolve, and under the taskpolicy branch below the failure surfaces as
# `taskpolicy: posix_spawn: No such file or directory` — a message that names
# neither the missing tool nor PATH, and reads like a build failure rather than a
# host misconfiguration. That killed a real validation run on 2026-08-15.
#
# Probing the caller's own shell is what makes this class hard to see: an
# interactive probe resolves `cmake` fine and reports the mode cannot recur while
# the dispatch shell still cannot find it. This check runs in the shell that will
# actually spawn the command, so it cannot give that false pass.
case "$1" in
  */*) [ -x "$1" ] || { log "build command not executable: $1"; exit 127; } ;;
  *)
    if ! command -v "$1" >/dev/null 2>&1; then
      log "build command not found on PATH: $1"
      log "PATH=$PATH"
      log "A Shipyard dispatch runs a non-interactive login shell, which does not"
      log "read an interactive profile — /opt/homebrew/bin is a common omission."
      exit 127
    fi
    ;;
esac

# Run the build as a CHILD (not exec) so the EXIT trap fires and the lease is
# released even on failure. Background QoS on laptop-class hosts keeps a shared
# machine's UI responsive (taskpolicy only re-prioritizes; the -j cap above is
# the real bound).
# Record the machine's state alongside the build's, so a failure carries the
# evidence needed to tell "your diff is broken" from "the host was melting".
#
# Without it the two are indistinguishable, and the tempting reading is the
# wrong one. Measured cases: an installed-SDK matrix that timed out at 1200s
# under load 163 and passed in 646s on the SAME commit once the host was quiet;
# a mac lane reporting `Stage 'configure' failed` after 3382s in configure —
# normally minutes — while GitHub's check passed on that same commit. Each of
# those cost a full diagnosis cycle, and the natural next step ("my change
# broke configure") sends someone editing correct code.
#
# The floor case matters most. Parallelism is negotiated ONCE, at startup, so a
# build that starts while the host is busy stays pinned at the floor for its
# entire life even after the host goes quiet — which is how a build that fits
# the cap on a quiet machine blows through it anyway.
load_average() {
  local raw
  if [ -r /proc/loadavg ]; then
    cut -d' ' -f1-3 < /proc/loadavg
    return
  fi
  # macOS: `vm.loadavg` prints `{ 1.23 4.56 7.89 }`.
  if raw="$(sysctl -n vm.loadavg 2>/dev/null)" && [ -n "$raw" ]; then
    echo "$raw" | tr -d '{}' | awk '{print $1, $2, $3}'
    return
  fi
  echo "unknown"
}

start_load="$(load_average)"
start_epoch="$(date +%s 2>/dev/null || echo 0)"

write_build_marker "$@"

rc=0
if [ "$qos" = "background" ] && command -v taskpolicy >/dev/null 2>&1 \
    && [ "${PULP_TARTCI_TASKPOLICY:-}" != "0" ]; then
  taskpolicy -b "$@" || rc=$?
else
  "$@" || rc=$?
fi

if [ "$rc" -ne 0 ]; then
  end_epoch="$(date +%s 2>/dev/null || echo 0)"
  elapsed=$(( end_epoch - start_epoch ))
  cores="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 0)"
  floor="$(min_jobs)"
  log "build failed rc=$rc after ${elapsed}s"
  log "  host: cores=$cores load(start)=[$start_load] load(now)=[$(load_average)]"
  log "  build: -j$jobs (floor=$floor) lease=${LEASE_ID:-none}"
  # One-minute load is the first field; compare it against the core count.
  now_one="$(load_average | awk '{print $1}')"
  contended=0
  if [ "$jobs" = "$floor" ]; then contended=1; fi
  if [ "$cores" -gt 0 ] 2>/dev/null && [ "$now_one" != "unknown" ]; then
    if awk -v l="$now_one" -v c="$cores" 'BEGIN { exit !(l > c * 1.5) }'; then
      contended=1
    fi
  fi
  if [ "$contended" -eq 1 ]; then
    log "  VERDICT: the host was contended (build pinned at the parallelism"
    log "  floor and/or load well above core count). Confirm against a quiet"
    log "  host before concluding the diff is at fault — this signature has"
    log "  passed on re-run with no code change."
  fi
fi
exit "$rc"
