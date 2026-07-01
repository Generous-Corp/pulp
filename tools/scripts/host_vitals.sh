#!/usr/bin/env bash
# host_vitals.sh — a cheap host-health probe shared by the agent loop, gates.sh,
# the pre-push hook, the tartci CI pool, and Shipyard, so all of them can back
# off BEFORE memory exhaustion trips the kernel jetsam killer and reboots the
# host.
#
# Why memory-pressure-primary (not load): the class of incident this guards
# against is *memory* exhaustion. When a Mac Studio co-hosts an interactive
# session + a heavy MCP stack + several CI runners, RAM fills, the compressor
# thrashes, jetsam starts killing processes, and the window server crashes into
# an unclean reboot — taking any in-flight required-gate CI job down with it. A
# high 1-minute load average is a *symptom* of that thrash (blocked processes
# piling up), not the cause. A healthy parallel build on a 28-core box
# legitimately drives load to 1-2x cores with normal memory pressure, so a
# load-only threshold would false-trip on every honest build. This probe keys
# CRITICAL/WARN off `kern.memorystatus_vm_pressure_level` and fresh JetsamEvent
# reports; load only ever *corroborates* a warn, never raises one alone.
#
# Levels map to exit codes so callers can gate on `$?` without parsing:
#     green    = 0    host is healthy; proceed
#     warn     = 10   host is under real pressure; shed optional load, prefer
#                     read-only work, don't pile on new heavy builds
#     critical = 20   host is shedding load (jetsam) or at critical pressure;
#                     refuse new heavy work, ship via auto-merge not a foreground
#                     watch, and (for a CI pool) stop pulling new jobs
#
# Usage:
#     host_vitals.sh            # one-line human summary; exit code = level
#     host_vitals.sh --json     # machine-readable JSON; exit code = level
#     host_vitals.sh --quiet    # no output; exit code = level
#     host_vitals.sh --level    # print just green|warn|critical; exit code = level
#
# Non-macOS hosts have no jetsam/pressure sysctl; the probe degrades to a
# load-only check (WARN above a high multiple of cores) and never reports
# CRITICAL, because the memory-death signal it exists to catch is macOS-specific.
#
# Test seams (all optional; production leaves them unset):
#     PULP_VITALS_SYSCTL       path to a stub replacing `sysctl`
#     PULP_VITALS_REPORTS_DIR  dir scanned for JetsamEvent-*/WindowServer-*.ips
#                              (default /Library/Logs/DiagnosticReports)
#     PULP_VITALS_NOW          epoch seconds treated as "now" for report ages
#     PULP_VITALS_UNAME        override the OS name (default `uname -s`)
#
# Thresholds (deliberately conservative to avoid false CRITICALs that would
# stall a required CI gate):
#     CRITICAL  pressure_level == 4 (critical)  OR  a JetsamEvent < 5 min old
#     WARN      pressure_level == 2 (warn)       OR  a JetsamEvent < 15 min old
#               OR a WindowServer *.ips crash < 15 min old
#               OR load1 > 3 x ncpu
#     GREEN     otherwise
set -euo pipefail

CRIT_JETSAM_SECS=300      # 5 min: a jetsam this fresh means "shedding load NOW"
WARN_JETSAM_SECS=900      # 15 min: recent enough to still be recovering
WARN_WINSERVER_SECS=900   # 15 min: a fresh WindowServer crash precedes reboot
WARN_LOAD_CORES_MULT=3    # load1 above 3x cores corroborates a warn

_os() { printf '%s' "${PULP_VITALS_UNAME:-$(uname -s)}"; }

_sysctl() {
  if [ -n "${PULP_VITALS_SYSCTL:-}" ]; then
    "${PULP_VITALS_SYSCTL}" "$@"
  else
    sysctl "$@"
  fi
}

_now() { printf '%s' "${PULP_VITALS_NOW:-$(date +%s)}"; }

_reports_dir() { printf '%s' "${PULP_VITALS_REPORTS_DIR:-/Library/Logs/DiagnosticReports}"; }

# ncpu, defaulting to 1 so the load multiple is never divided by zero.
_ncpu() {
  local n
  n="$(_sysctl -n hw.ncpu 2>/dev/null || echo 1)"
  case "$n" in ''|*[!0-9]*) n=1 ;; esac
  printf '%s' "$n"
}

# 1-minute load average. macOS: `sysctl -n vm.loadavg` -> "{ 7.28 13.58 16.74 }".
# Falls back to `uptime` parsing when vm.loadavg is unavailable.
_load1() {
  local raw
  raw="$(_sysctl -n vm.loadavg 2>/dev/null || true)"
  if [ -n "$raw" ]; then
    printf '%s' "$raw" | awk '{ for (i=1;i<=NF;i++) if ($i ~ /^[0-9]+\.[0-9]+$/) { print $i; exit } }'
    return
  fi
  uptime 2>/dev/null | sed -E 's/.*load averages?: *//; s/,.*//' | awk '{print $1}'
}

# macOS memory-pressure level: 1 normal, 2 warn, 4 critical. Absent -> 1.
_pressure_level() {
  local p
  p="$(_sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)"
  case "$p" in ''|*[!0-9]*) p=1 ;; esac
  printf '%s' "$p"
}

# Age in seconds of the newest report file matching a glob, or empty if none.
# Uses BSD `stat -f %m` (macOS); on other OSes returns empty (no such reports).
_newest_report_age() {
  local glob="$1" dir now newest_mtime age
  dir="$(_reports_dir)"
  [ -d "$dir" ] || { printf ''; return; }
  now="$(_now)"
  newest_mtime=""
  # shellcheck disable=SC2044
  for f in $(find "$dir" -maxdepth 1 -type f -name "$glob" 2>/dev/null); do
    local m
    m="$(stat -f %m "$f" 2>/dev/null || echo '')"
    [ -n "$m" ] || continue
    if [ -z "$newest_mtime" ] || [ "$m" -gt "$newest_mtime" ]; then
      newest_mtime="$m"
    fi
  done
  [ -n "$newest_mtime" ] || { printf ''; return; }
  age=$(( now - newest_mtime ))
  [ "$age" -lt 0 ] && age=0
  printf '%s' "$age"
}

# Integer compare "a > b*mult" without bc: compares floor(load1) to cores*mult.
_load_over() {
  local load1="$1" ncpu="$2" mult="$3" load_int threshold
  load_int="${load1%%.*}"
  case "$load_int" in ''|*[!0-9]*) load_int=0 ;; esac
  threshold=$(( ncpu * mult ))
  [ "$load_int" -gt "$threshold" ]
}

main() {
  local mode="human"
  case "${1:-}" in
    --json) mode="json" ;;
    --quiet) mode="quiet" ;;
    --level) mode="level" ;;
    ''|--human) mode="human" ;;
    -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "host_vitals.sh: unknown arg '$1'" >&2; exit 2 ;;
  esac

  local os ncpu load1 pressure jetsam_age winserver_age
  os="$(_os)"
  ncpu="$(_ncpu)"
  load1="$(_load1)"
  [ -n "$load1" ] || load1="0.0"

  local level="green" reason="healthy"

  if [ "$os" = "Darwin" ]; then
    pressure="$(_pressure_level)"
    jetsam_age="$(_newest_report_age 'JetsamEvent-*')"
    winserver_age="$(_newest_report_age 'WindowServer-*.ips')"

    if [ "$pressure" = "4" ]; then
      level="critical"; reason="memory pressure critical (level 4)"
    elif [ -n "$jetsam_age" ] && [ "$jetsam_age" -lt "$CRIT_JETSAM_SECS" ]; then
      level="critical"; reason="jetsam ${jetsam_age}s ago (host shedding load)"
    elif [ "$pressure" = "2" ]; then
      level="warn"; reason="memory pressure warning (level 2)"
    elif [ -n "$jetsam_age" ] && [ "$jetsam_age" -lt "$WARN_JETSAM_SECS" ]; then
      level="warn"; reason="jetsam ${jetsam_age}s ago (recovering)"
    elif [ -n "$winserver_age" ] && [ "$winserver_age" -lt "$WARN_WINSERVER_SECS" ]; then
      level="warn"; reason="WindowServer crash ${winserver_age}s ago"
    elif _load_over "$load1" "$ncpu" "$WARN_LOAD_CORES_MULT"; then
      level="warn"; reason="load ${load1} > ${WARN_LOAD_CORES_MULT}x${ncpu} cores"
    fi
  else
    # Non-macOS: no jetsam/pressure signal — load-only WARN, never CRITICAL.
    pressure="n/a"; jetsam_age=""; winserver_age=""
    if _load_over "$load1" "$ncpu" "$WARN_LOAD_CORES_MULT"; then
      level="warn"; reason="load ${load1} > ${WARN_LOAD_CORES_MULT}x${ncpu} cores"
    fi
  fi

  local code=0
  case "$level" in
    green) code=0 ;;
    warn) code=10 ;;
    critical) code=20 ;;
  esac

  case "$mode" in
    quiet) : ;;
    level) printf '%s\n' "$level" ;;
    json)
      printf '{"level":"%s","code":%d,"reason":"%s","os":"%s","ncpu":%s,"load1":"%s","pressure_level":"%s","jetsam_age_s":%s,"windowserver_age_s":%s}\n' \
        "$level" "$code" "$reason" "$os" "$ncpu" "$load1" "$pressure" \
        "${jetsam_age:-null}" "${winserver_age:-null}"
      ;;
    human)
      printf 'host_vitals: %s — %s (host=%s load1=%s cores=%s pressure=%s)\n' \
        "$(printf '%s' "$level" | tr '[:lower:]' '[:upper:]')" \
        "$reason" "$(hostname -s 2>/dev/null || hostname)" "$load1" "$ncpu" "$pressure"
      ;;
  esac

  exit "$code"
}

main "$@"
