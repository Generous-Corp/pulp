#!/usr/bin/env bash
#
# disk_pressure_respond.sh — turn a tartci disk-admission refusal into
# reclaimed space, or into an itemised escalation.
#
# THE GAP THIS CLOSES
#
# When a CI supervisor refuses to start a runner VM because the host is below
# its disk floor, it already writes a precise, machine-readable refusal:
#
#   {"kind":"tartci.disk-admission","status":"denied",
#    "reason":"disk_capacity_insufficient","free_bytes":...,"floor_bytes":...}
#
# The detection was never the problem. Nothing consumed that record, so a
# 10 GiB shortfall stalled a release for 90+ minutes until a human noticed a
# full disk and freed space by hand. This script is the consumer.
#
# WHAT IT DOES
#
#   1. Reads the newest disk-admission receipt and requires it to be a FRESH,
#      exact disk-axis denial. A stale or non-disk refusal is not authority.
#   2. Re-measures free space itself. The receipt says what was true when the
#      VM was refused; reclaim must act on what is true now.
#   3. Reclaims in escalating order, cheapest and least contentious first,
#      re-measuring between steps, and STOPS as soon as the floor plus a
#      margin is cleared. It does not free everything it could.
#   4. Reports REMOVALS, never a free-space delta: other agents build on this
#      machine concurrently, so a delta moves in both directions and is not a
#      measurement of what this script did.
#   5. If the safe reclaim cannot reach the floor, it declines and itemises
#      what it refused and why, and inventories stale VM images as a REPORT.
#      VM images are fleet infrastructure; this script never deletes one.
#
# WHAT IT WILL NOT DO
#
#   Every removal is delegated to an existing, individually gated reaper. This
#   script adds no deletion logic of its own. It never touches unmerged work,
#   a dirty worktree, a live build, a primary checkout, or anything outside a
#   registered worktree of this repository. The worst case for any action it
#   takes is a rebuild.
#
# Usage:
#   disk_pressure_respond.sh                 # dry-run: what it would reclaim
#   disk_pressure_respond.sh --apply         # reclaim up to the floor + margin
#   disk_pressure_respond.sh --receipt-dir D --margin-gib 10 --max-age-secs 900
#   disk_pressure_respond.sh --free-bytes N --floor-bytes M --probe-path P
#                                            # drive it without a receipt
#
# Exit codes:
#   0  nothing to do, or the floor was cleared
#   1  usage / environment error
#   2  no fresh, actionable disk-capacity refusal found
#   3  reclaim ran but could NOT reach the floor (escalation; see the report)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

RECEIPT_DIR="${TARTCI_DISK_DENIAL_RECEIPT_DIR:-${HOME}/.tartci/state/disk-admission}"
MARGIN_GIB="${PULP_DISK_RESPOND_MARGIN_GIB:-10}"
MAX_AGE_SECS="${PULP_DISK_RESPOND_MAX_AGE_SECS:-900}"
APPLY=0
FREE_BYTES=""; FLOOR_BYTES=""; PROBE_PATH=""
REPORT_JSON="${PULP_DISK_RESPOND_REPORT:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --apply) APPLY=1 ;;
        --dry-run) APPLY=0 ;;
        --receipt-dir) RECEIPT_DIR="${2:?}"; shift ;;
        --margin-gib) MARGIN_GIB="${2:?}"; shift ;;
        --max-age-secs) MAX_AGE_SECS="${2:?}"; shift ;;
        --free-bytes) FREE_BYTES="${2:?}"; shift ;;
        --floor-bytes) FLOOR_BYTES="${2:?}"; shift ;;
        --probe-path) PROBE_PATH="${2:?}"; shift ;;
        --report-json) REPORT_JSON="${2:?}"; shift ;;
        -h|--help) sed -n '2,60p' "$0"; exit 0 ;;
        *) echo "disk_pressure_respond: unknown argument '$1'" >&2; exit 1 ;;
    esac
    shift
done

case "${MARGIN_GIB}" in ''|*[!0-9]*) echo "disk_pressure_respond: --margin-gib must be an integer" >&2; exit 1 ;; esac
case "${MAX_AGE_SECS}" in ''|*[!0-9]*) echo "disk_pressure_respond: --max-age-secs must be an integer" >&2; exit 1 ;; esac

note() { echo "disk_pressure_respond: $*"; }

# ── 1. Establish an actionable refusal ─────────────────────────────────────
# Either explicitly supplied, or read from the newest receipt. A receipt is
# only authority when it is an exact disk-axis denial AND recent: a refusal
# from an hour ago describes a condition that may already be resolved, and
# acting on it would delete against a shortfall that no longer exists.
SOURCE="flags"
if [ -z "${FREE_BYTES}" ] || [ -z "${FLOOR_BYTES}" ]; then
    SOURCE="receipt"
    if [ ! -d "${RECEIPT_DIR}" ]; then
        note "no receipt directory at ${RECEIPT_DIR}; nothing to consume."
        exit 2
    fi
    parsed="$(/usr/bin/python3 - "${RECEIPT_DIR}" "${MAX_AGE_SECS}" <<'PY'
import datetime as dt, json, pathlib, sys
receipt_dir, max_age = pathlib.Path(sys.argv[1]), int(sys.argv[2])
now = dt.datetime.now(dt.timezone.utc)
best = None
for path in receipt_dir.glob("*.json"):
    try:
        record = json.loads(path.read_text())
    except (OSError, ValueError):
        continue
    if not isinstance(record, dict) or record.get("kind") != "tartci.disk-admission":
        continue
    if record.get("status") != "denied" or record.get("reason") != "disk_capacity_insufficient":
        continue
    stamp = record.get("observed_at")
    try:
        seen = dt.datetime.fromisoformat(str(stamp).replace("Z", "+00:00"))
    except ValueError:
        continue
    if seen.tzinfo is None:
        continue
    age = (now - seen).total_seconds()
    if age < 0 or age > max_age:
        continue
    free, floor = record.get("free_bytes"), record.get("floor_bytes")
    required = record.get("required_bytes")
    if type(free) is not int or type(floor) is not int:
        continue
    target = required if type(required) is int and required >= floor else floor
    if best is None or seen > best[0]:
        best = (seen, free, target, record.get("probe_path") or "",
                record.get("runner") or "?", record.get("lane") or "?", int(age))
if best is None:
    raise SystemExit(4)
_, free, target, probe, runner, lane, age = best
print(f"{free}\t{target}\t{probe}\t{runner}\t{lane}\t{age}")
PY
)" || {
        note "no fresh disk_capacity_insufficient refusal in ${RECEIPT_DIR} (within ${MAX_AGE_SECS}s)."
        note "a stale or non-disk refusal is not authority to delete anything."
        exit 2
    }
    IFS=$'\t' read -r FREE_BYTES FLOOR_BYTES PROBE_PATH RUNNER LANE AGE <<<"${parsed}"
    note "consuming refusal: runner=${RUNNER} lane=${LANE} age=${AGE}s"
fi

: "${PROBE_PATH:=${HOME}}"
[ -d "${PROBE_PATH}" ] || PROBE_PATH="${HOME}"

gib() { awk -v b="$1" 'BEGIN{printf "%.2f", b/1024/1024/1024}'; }

# ── 2. Re-measure. The receipt is a claim about the past. ──────────────────
free_now() {
    local kb; kb="$(df -Pk "${PROBE_PATH}" 2>/dev/null | awk 'NR==2 {print $4}')"
    case "${kb}" in ''|*[!0-9]*) return 1 ;; esac
    echo $(( kb * 1024 ))
}
CURRENT="$(free_now)" || { note "cannot measure free space on ${PROBE_PATH}; refusing to act."; exit 1; }
TARGET=$(( FLOOR_BYTES + MARGIN_GIB * 1024 * 1024 * 1024 ))

note "probe=${PROBE_PATH}"
note "refusal free=$(gib "${FREE_BYTES}") GiB  floor=$(gib "${FLOOR_BYTES}") GiB (source: ${SOURCE})"
note "measured now free=$(gib "${CURRENT}") GiB  target=$(gib "${TARGET}") GiB (floor + ${MARGIN_GIB} GiB margin)"

if [ "${CURRENT}" -ge "${TARGET}" ]; then
    note "already above the target; the shortfall resolved without reclaim. Nothing removed."
    exit 0
fi
note "SHORTFALL against target = $(gib $(( TARGET - CURRENT ))) GiB"
[ "${APPLY}" -eq 1 ] || note "DRY RUN — pass --apply to reclaim. Reapers below also run in dry-run."

# ── 3. Escalating reclaim, re-measured between steps, stopping at target ───
REMOVED_LOG="$(mktemp -t dpr-removed)"; : > "${REMOVED_LOG}"
STEPS_RUN=""
trap 'rm -f "${REMOVED_LOG}"' EXIT

run_step() {
    local label="$1"; shift
    CURRENT="$(free_now)" || return 0
    if [ "${CURRENT}" -ge "${TARGET}" ]; then
        note "target met before '${label}'; stopping. Reclaim targets the floor, not the maximum."
        return 0
    fi
    note "── step: ${label}"
    STEPS_RUN="${STEPS_RUN}${label}|"
    "$@" 2>&1 | tee -a "${REMOVED_LOG}" | sed 's/^/    /'
    return 0
}

reap_coverage() {
    local script="${REPO_ROOT}/tools/scripts/clean_build_cov.sh"
    [ -x "${script}" ] || { echo "  (clean_build_cov.sh not present; skipped)"; return 0; }
    if [ "${APPLY}" -eq 1 ]; then "${script}" --yes; else "${script}"; fi
}

reap_worktree_builds() {
    local script="${REPO_ROOT}/tools/scripts/clean_worktree_builds.sh"
    [ -x "${script}" ] || { echo "  (clean_worktree_builds.sh not present; skipped)"; return 0; }
    # Hand the reaper the exact stopping condition so it reclaims up to the
    # target and then stops, rather than freeing every eligible byte.
    if [ "${APPLY}" -eq 1 ]; then
        PULP_REAP_STOP_AT_FREE_BYTES="${TARGET}" PULP_REAP_STOP_AT_PATH="${PROBE_PATH}" \
            "${script}" --yes
    else
        PULP_REAP_STOP_AT_FREE_BYTES="${TARGET}" PULP_REAP_STOP_AT_PATH="${PROBE_PATH}" \
            "${script}" --verbose
    fi
}

run_step "coverage scratch (clean_build_cov.sh)" reap_coverage
run_step "merged worktree build artifacts (clean_worktree_builds.sh)" reap_worktree_builds

# ── 4. Report removals, never a free-space delta ──────────────────────────
REMOVED_COUNT="$(grep -cE '^[[:space:]]*(removed|would remove) ' "${REMOVED_LOG}" 2>/dev/null || true)"
REMOVED_COUNT="${REMOVED_COUNT:-0}"
CURRENT="$(free_now)" || CURRENT=0

echo
note "── outcome"
note "reclaim actions: ${REMOVED_COUNT} $([ "${APPLY}" -eq 1 ] && echo 'removal(s)' || echo 'removal(s) proposed')"
note "free now $(gib "${CURRENT}") GiB against target $(gib "${TARGET}") GiB"
note "(free space is reported for context only — other builds run concurrently,"
note " so a free-space delta is not a measurement of what this script removed.)"

RC=0
if [ "${APPLY}" -eq 1 ] && [ "${CURRENT}" -lt "${TARGET}" ]; then
    RC=3
    echo
    note "ESCALATION — safe reclaim could not reach the floor."
    note "Declined, with reasons (from the reapers above):"
    grep -oE 'keeping \([^)]*\)' "${REMOVED_LOG}" 2>/dev/null | sort | uniq -c | sort -rn | sed 's/^/    /'
    echo
    note "Remaining lever: stale Tart VM images. These are FLEET INFRASTRUCTURE."
    note "Inventoried, never deleted automatically — an operator decides:"
    if command -v tart >/dev/null 2>&1; then
        tart list 2>/dev/null | awk 'NR==1 || $NF=="stopped"' | sed 's/^/    /'
    else
        note "    (tart not on PATH; cannot inventory)"
    fi
fi

if [ -n "${REPORT_JSON}" ]; then
    /usr/bin/python3 - "${REPORT_JSON}" "${APPLY}" "${FREE_BYTES}" "${FLOOR_BYTES}" \
        "${TARGET}" "${CURRENT}" "${REMOVED_COUNT}" "${PROBE_PATH}" "${STEPS_RUN}" "${RC}" <<'PY'
import datetime as dt, json, os, pathlib, sys, tempfile
(out, apply_flag, free_at_refusal, floor_b, target, current,
 removed, probe, steps, rc) = sys.argv[1:11]
record = {
    "schema_version": 1, "kind": "pulp.disk-pressure-response",
    "observed_at": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
    "apply": apply_flag == "1", "probe_path": probe,
    "free_bytes_at_refusal": int(free_at_refusal), "floor_bytes": int(floor_b),
    "target_bytes": int(target), "free_bytes_after": int(current),
    "removal_actions": int(removed), "steps": [s for s in steps.split("|") if s],
    "outcome": {"0": "target_met_or_no_action", "3": "escalated_floor_not_reached"}.get(rc, "unknown"),
}
path = pathlib.Path(out); path.parent.mkdir(parents=True, exist_ok=True)
fd, name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
with os.fdopen(fd, "w") as handle:
    json.dump(record, handle, indent=2, sort_keys=True); handle.write("\n")
os.replace(name, path)
PY
    note "report written to ${REPORT_JSON}"
fi
exit "${RC}"
