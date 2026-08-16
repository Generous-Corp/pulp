#!/usr/bin/env bash
# Interrupted-build guard for warm, reused build directories.
#
# A build killed mid-compile leaves partial object files behind. The next
# incremental build links those against freshly compiled ones, mixing object
# layouts, and the result is heap corruption and SEGFAULTs in tests that have
# nothing to do with the change under validation. That failure reads exactly
# like a bad diff, which is what makes it expensive: on 2026-06-07 it reddened
# every open PR's gate after branch churn while the hosted lane stayed green.
#
# `build.yml`'s macOS lane has carried this guard since. Shipyard's local mac
# target — same machine, same warm `build/`, and the lane that actually gets
# killed by a timeout — had none, so each timeout seeded the next run's
# failure and the loop sustained itself once it started.
#
# Two properties matter and neither is negotiable:
#
#   * It is a FILE, not a trap or an exit handler. The kill here is a timeout
#     SIGKILL; nothing runs at failure time by definition. Anything that must
#     execute to record the failure is structurally blind to it. A file written
#     beforehand and removed afterwards leaves the marker behind by default.
#
#   * It is armed BEFORE configure, not after. `build.yml` arms it after its
#     configure step, which leaves a run killed *during* configure
#     unrecorded — and a half-written CMakeCache.txt is its own kind of broken.
#
# Ordering note for anyone porting this elsewhere: arming without a timeout the
# build can actually finish under produces an infinite
# wipe → cold rebuild → timeout → wipe loop, each cycle burning an hour of a
# shared machine. This is safe here only because `[targets.mac] timeout_secs`
# was raised to 7200 first. Do not lift the sentinel into a lane without
# checking that the cap and the granted `-j` are reconciled.
#
# A cleanly FAILED stage is not an interrupted one, and the difference matters.
# A configure that exits non-zero — a dependency-floor mismatch, a missing
# toolchain — produced no object files at all, so wiping its build dir buys
# nothing and costs a cold rebuild, which on this lane is exactly what pushes
# the next run over the cap. `guard` therefore clears the marker when its
# command exits non-zero of its own accord, and leaves it when the command is
# killed. That discrimination needs no extra bookkeeping: it falls out of the
# same SIGKILL property the marker relies on, because nothing after the command
# runs when the process is killed.
#
#   build-dir-sentinel.sh arm   <dir>            # wipe if stale, then mark
#   build-dir-sentinel.sh clear <dir>            # build finished cleanly
#   build-dir-sentinel.sh guard <dir> <command>  # arm, run, clear iff it FAILED
set -euo pipefail

verb="${1:-}"
dir="${2:-}"

if [ -z "$verb" ] || [ -z "$dir" ]; then
  echo "usage: build-dir-sentinel.sh {arm|clear|guard} <build-dir> [command]" >&2
  exit 2
fi

sentinel="$dir/.pulp-build-incomplete"

case "$verb" in
  arm)
    if [ -f "$sentinel" ]; then
      echo "build-dir-sentinel: $dir was left mid-build by an interrupted run;" \
           "recreating it rather than linking against partial objects"
      rm -rf "$dir"
    fi
    mkdir -p "$dir"
    : > "$sentinel"
    ;;
  clear)
    rm -f "$sentinel"
    ;;
  guard)
    command="${3:-}"
    if [ -z "$command" ]; then
      echo "build-dir-sentinel: guard needs a command to run" >&2
      exit 2
    fi
    "$0" arm "$dir"
    rc=0
    sh -c "$command" || rc=$?
    # A shell reports a signal death as 128+signum. Anything at or above 128 was
    # therefore KILLED, not finished, and must stay armed — the timeout may kill
    # only the child and leave this wrapper alive to run the line below, so
    # "we reached this point" is not by itself evidence of a clean exit.
    if [ "$rc" -ne 0 ] && [ "$rc" -lt 128 ]; then
      # Exited on its own, so nothing is half-written. Keep the warm dir.
      rm -f "$sentinel"
    fi
    exit "$rc"
    ;;
  *)
    echo "build-dir-sentinel: unknown verb '$verb' (want arm, clear, or guard)" >&2
    exit 2
    ;;
esac
