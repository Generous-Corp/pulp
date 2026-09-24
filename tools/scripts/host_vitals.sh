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
#     host_vitals.sh --build-json  # build-capacity snapshot (ccache, gate VMs,
#                                  # tartci leases/commit, wheelhouse); exit 0
#
# --build-json is deliberately NOT part of the cheap probe: it shells out to
# ccache, ps and tartci (~0.5 s), so only the launchd sensor tick and the
# build-speed scorecard call it. Every field it cannot read is null, never 0,
# so a missing tool never reads as an empty cache or an idle host.
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
#     PULP_VITALS_TOOL_PATH    dirs searched for ccache/tartci by --build-json
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

# --- build-capacity snapshot (--build-json) ----------------------------------
# JSON string escaping for the handful of free-text fields.
_jstr() {
  local v="${1-}"
  v="${v//\\/\\\\}"; v="${v//\"/\\\"}"
  printf '"%s"' "$v"
}

_num_or_null() {
  case "${1:-}" in ''|*[!0-9.]*) printf 'null' ;; *) printf '%s' "$1" ;; esac
}

# Resolve a tool even from launchd's or ssh's minimal PATH. PULP_VITALS_TOOL_PATH
# (a colon-separated dir list) replaces the whole search, for tests.
_find_tool() {
  local name="$1" c d
  if [ -n "${PULP_VITALS_TOOL_PATH+set}" ]; then
    local IFS=:
    for d in $PULP_VITALS_TOOL_PATH; do
      [ -n "$d" ] && [ -x "$d/$name" ] && { printf '%s' "$d/$name"; return 0; }
    done
    return 1
  fi
  for c in "$(command -v "$name" 2>/dev/null)" "/opt/homebrew/bin/$name" "/usr/local/bin/$name" \
           "$HOME/.local/bin/$name"; do
    [ -n "$c" ] && [ -x "$c" ] && { printf '%s' "$c"; return 0; }
  done
  return 1
}

# ccache stats for one cache dir ("" = the user's configured cache) as JSON.
_ccache_json() {
  local bin="$1" dir="$2" stats cdir
  if [ -n "$dir" ]; then
    stats="$("$bin" -d "$dir" -s 2>/dev/null)" || { printf 'null'; return; }
    cdir="$dir"
  else
    stats="$("$bin" -s 2>/dev/null)" || { printf 'null'; return; }
    cdir="$("$bin" -p 2>/dev/null | awk -F' = ' '/ cache_dir = / {print $2; exit}')"
  fi
  printf '%s\n' "$stats" | awk -v cdir="$cdir" '
    function pct(line,   a) { split(line, a, "("); sub(/%\).*/, "", a[2]); return a[2] + 0 }
    /^  Hits:/ && hit == "" { hit = pct($0) }
    /^Uncacheable calls:/ { unc = pct($0) }
    /^  Cache size/ { gsub(/[()%]/, " "); n = 0
                      for (i = 1; i <= NF; i++) if ($i ~ /^[0-9.]+$/) v[++n] = $i
                      size = v[1]; max = v[2] }
    /^  Cleanups:/ { cleanups = $2 }
    END {
      gsub(/"/, "", cdir)
      printf "{\"dir\":\"%s\",\"hit_pct\":%s,\"size_gb\":%s,\"max_gb\":%s,\"cleanups\":%s,\"uncacheable_pct\":%s}",
        cdir, (hit == "" ? "null" : hit), (size == "" ? "null" : size + 0),
        (max == "" ? "null" : max + 0), (cleanups == "" ? "null" : cleanups + 0),
        (unc == "" ? "null" : unc)
    }'
}

build_json() {
  # Every probe below may legitimately find nothing (no tartci, no VMs, no
  # ccache); a non-matching grep must yield null, not abort the snapshot.
  set +e +o pipefail
  local host ncpu mem load free_pct swap_used ccache_bin tartci_bin
  host="$(hostname -s 2>/dev/null || hostname)"
  ncpu="$(_ncpu)"
  mem="$(_sysctl -n hw.memsize 2>/dev/null || echo '')"
  load="$(_sysctl -n vm.loadavg 2>/dev/null | tr -d '{}' | awk '{printf "[%s,%s,%s]", $1, $2, $3}')"
  [ -n "$load" ] || load="null"
  free_pct="$(memory_pressure -Q 2>/dev/null | awk -F': ' '/free percentage/ {gsub(/%/, "", $2); print $2}')"
  swap_used="$(_sysctl -n vm.swapusage 2>/dev/null | awk '{for (i=1;i<=NF;i++) if ($i=="used") {v=$(i+2); sub(/M$/, "", v); print v}}')"

  local ccache_host="null" ccache_gate="null"
  if ccache_bin="$(_find_tool ccache)"; then
    ccache_host="$(_ccache_json "$ccache_bin" "")"
    [ -d "$HOME/.cache/pulp-ci/ccache" ] && ccache_gate="$(_ccache_json "$ccache_bin" "$HOME/.cache/pulp-ci/ccache")"
  fi

  # Gate VMs: the Virtualization XPC service is the process that holds a VM's
  # memory; `tart run` is a thin launcher, so its RSS says nothing about the VM.
  local ps_out vms
  ps_out="$(ps -axo rss=,pcpu=,args= 2>/dev/null || true)"
  vms="$(printf '%s\n' "$ps_out" | awk '
    /com\.apple\.Virtualization\.VirtualMachine$/ { n++; rss+=$1; cpu+=$2 }
    /(^|\/)tart run / { name=$NF; names = names (names==""?"":",") "\"" name "\"" }
    END { printf "{\"count\":%d,\"rss_mb\":%d,\"cpu_pct\":%.0f,\"names\":[%s]}", n, rss/1024, cpu, names }')"
  local generation
  generation="$(printf '%s\n' "$ps_out" | grep -o 'tartci-generations/[0-9a-f]\{7,40\}' | head -1 | sed 's|.*/||' | cut -c1-12 || true)"

  local checkout="" leases="null" profile="null"
  checkout="$(git -C "$HOME/Code/tartci" rev-parse --short=12 HEAD 2>/dev/null || true)"
  if tartci_bin="$(_find_tool tartci)"; then
    leases="$("$tartci_bin" leases status --json 2>/dev/null | tr -d '\n' | sed -E 's/.*"capacity": *(\{[^}]*\}).*/\1/')"
    case "$leases" in '{'*'}') : ;; *) leases="null" ;; esac
    profile="$("$tartci_bin" host-profile --json 2>/dev/null | tr -d '\n' | sed -E 's/"notes": *\[[^]]*\],?//; s/"host": *\{[^}]*\},?//')"
    case "$profile" in '{'*'}') : ;; *) profile="null" ;; esac
  fi

  local wheels="null" wh="$HOME/.cache/pulp-ci/pip-wheelhouse"
  if [ -d "$wh" ]; then
    wheels="$(find "$wh" -maxdepth 2 -name '*.whl' 2>/dev/null | wc -l | tr -d ' ')"
  fi

  printf '{"host":%s,"ncpu":%s,"mem_bytes":%s,"load":%s,"memory_free_pct":%s,"swap_used_mb":%s,' \
    "$(_jstr "$host")" "$(_num_or_null "$ncpu")" "$(_num_or_null "$mem")" "$load" \
    "$(_num_or_null "$free_pct")" "$(_num_or_null "$swap_used")"
  printf '"ccache_host":%s,"ccache_gate":%s,"gate_vms":%s,' "$ccache_host" "$ccache_gate" "$vms"
  printf '"tartci":{"executing_generation":%s,"checkout_head":%s,"leases":%s,"host_profile":%s},' \
    "$( [ -n "$generation" ] && _jstr "$generation" || printf 'null')" \
    "$( [ -n "$checkout" ] && _jstr "$checkout" || printf 'null')" "$leases" "$profile"
  printf '"wheelhouse_wheels":%s,"sampled_at":%s}\n' "$wheels" "$(_now)"
}

main() {
  local mode="human"
  case "${1:-}" in
    --json) mode="json" ;;
    --quiet) mode="quiet" ;;
    --level) mode="level" ;;
    --build-json) build_json; exit 0 ;;
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
      # `sampled_at` is the epoch used to compute the ages above, so a consumer
      # can reconstruct an incident's absolute time as `sampled_at - age_s`
      # without trusting the file mtime (which a touch/copy could drift).
      printf '{"level":"%s","code":%d,"reason":"%s","os":"%s","ncpu":%s,"load1":"%s","pressure_level":"%s","jetsam_age_s":%s,"windowserver_age_s":%s,"sampled_at":%s}\n' \
        "$level" "$code" "$reason" "$os" "$ncpu" "$load1" "$pressure" \
        "${jetsam_age:-null}" "${winserver_age:-null}" "$(_now)"
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
