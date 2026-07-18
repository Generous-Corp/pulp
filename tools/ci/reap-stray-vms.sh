#!/usr/bin/env bash
# reap-stray-vms.sh — delete stray Tart VMs that starve the CI/release lanes.
#
# WHY: ad-hoc verification VMs and orphaned ephemeral clones accumulate and
# consume the host's 2-VM macOS capacity, so the release/gate supervisors hit
# `capacity_exceeded` and jobs queue forever (this stalled the v0.653.0 release).
# `tartci doctor --reap` only cleans STATE it tracks; it misses orphaned VMs and
# off-convention scratch names (e.g. `wam-cleanbuild`). This is the thorough,
# safe backstop, meant to run on a launchd timer.
#
# SAFETY — never delete a golden or an active job VM:
#   - KEEP: any tag (goldens like `pulp-build-runner:latest`), the explicit KEEP
#     list, and any VM whose name matches an ONLINE self-hosted GitHub runner
#     (it is actively serving a job right now).
#   - REAP: only names matching the ephemeral/scratch REAP families. By default
#     only STOPPED ones (a scheduled run must not kill an in-flight scratch
#     build). Pass --include-running to also reap RUNNING scratch VMs that are
#     NOT backing an online runner (manual aggressive cleanup, e.g. to unstick a
#     capacity_exceeded release).
#
# Usage: reap-stray-vms.sh [--fix] [--include-running]
#   (default: DRY-RUN — prints what it would do)
set -uo pipefail

# The store must come from the host. A wrong store is not a loud failure here —
# `tart list` against it simply returns nothing, so this reaper would inspect an
# empty universe, reap nothing, and exit 0 reporting success.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/tart-home.sh"
pulp_require_tart_home

FIX=0; INCLUDE_RUNNING=0
for a in "$@"; do
  case "$a" in
    --fix) FIX=1;;
    --include-running) INCLUDE_RUNNING=1;;
  esac
done

# Goldens / persistent VMs — NEVER touch (tags are also kept by the ':' rule).
KEEP_EXACT="pulp-vm rosetta-probe choc-dnd-linux macos-build-base macos-apple-xcode pulp-build-base pulp-build-runner pulp-intel-build pulp-linux-build pulp-windows-build"
# Ephemeral / scratch families safe to reap when stale.
REAP_RE='(-ephr-|^ephr-|scratch|-xbuild-|cleanbuild|-wip-|-tmp-)'

# VMs currently backing an ONLINE runner (actively serving a job) — keep.
ACTIVE="$(gh api "repos/danielraffel/pulp/actions/runners?per_page=100" \
  --jq '.runners[] | select(.status=="online") | .name' 2>/dev/null || true)"

echo "reap-stray-vms: TART_HOME=$TART_HOME fix=$FIX include_running=$INCLUDE_RUNNING"
tart list 2>/dev/null | awk 'NR>1{print $2, $NF}' | while read -r name state; do
  [ -z "$name" ] && continue
  case "$name" in *:*) continue;; esac                       # tagged golden
  for k in $KEEP_EXACT; do [ "$name" = "$k" ] && continue 2; done
  printf '%s\n' "$ACTIVE" | grep -qxF "$name" && continue    # serving a job
  echo "$name" | grep -qE "$REAP_RE" || continue             # not a scratch family

  reap=0; why=""
  if [ "$state" = "stopped" ]; then
    reap=1; why="stopped stray"
  elif [ "$state" = "running" ] && [ "$INCLUDE_RUNNING" = "1" ]; then
    reap=1; why="running scratch, no online runner"
  fi
  [ "$reap" = "0" ] && continue

  if [ "$FIX" = "1" ]; then
    tart stop "$name" >/dev/null 2>&1 || true
    tart delete "$name" >/dev/null 2>&1 && echo "REAPED ($why): $name" || echo "FAILED: $name"
  else
    echo "would reap ($why): $name"
  fi
done
echo "reap-stray-vms: $([ "$FIX" = 1 ] && echo done || echo DRY-RUN)"
