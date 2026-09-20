#!/usr/bin/env bash
#
# test_disk_pressure_respond.sh — negative controls for the disk-pressure
# reclaim path.
#
# Every gate here is proven by CONSTRUCTING the condition it must refuse and
# showing the refusal, then REMOVING that condition and showing the same
# directory become reapable. A gate that only ever passes is measuring
# nothing; a refusal with no matching acceptance could be a broken instrument
# refusing everything. Each case therefore reports a pair.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REAPER="${SCRIPT_DIR}/clean_worktree_builds.sh"
RESPONDER="${SCRIPT_DIR}/disk_pressure_respond.sh"
PASS=0; FAIL=0
LAB="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/dpr-test-XXXXXX")" && pwd -P)"
cleanup() {
    [ -n "${LIVE_PID:-}" ] && kill "${LIVE_PID}" 2>/dev/null
    chmod -R u+w "${LAB}" 2>/dev/null
    rm -rf "${LAB}"
}
trap cleanup EXIT

ok()   { PASS=$((PASS+1)); echo "  PASS  $*"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL  $*"; }

# verdict <worktree-name> <artifact> -> prints "REAPABLE" or the keep reason
verdict() {
    local name="$1" artifact="$2" line
    line="$(cd "${LAB}/repo" && "${LAB}/repo/tools/scripts/clean_worktree_builds.sh" --verbose 2>&1 \
        | grep -E '(would remove|keeping \()' \
        | grep -E "/${name}/${artifact}([[:space:]]|\$)" | head -1)"
    case "${line}" in
        *"would remove"*) echo "REAPABLE" ;;
        *"keeping ("*)    echo "${line#*keeping (}" | sed 's/).*//' ;;
        "")               echo "NOT-CONSIDERED" ;;
        *)                echo "UNKNOWN: ${line}" ;;
    esac
}

expect() {  # expect <label> <got> <want-substring>
    local label="$1" got="$2" want="$3"
    case "${got}" in
        *"${want}"*) ok "${label}  ->  ${got}" ;;
        *)           bad "${label}  ->  got '${got}', wanted '${want}'" ;;
    esac
}

echo "== building lab at ${LAB}"
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null GIT_TERMINAL_PROMPT=0
git init -q --bare "${LAB}/origin.git"
git init -q -b main "${LAB}/repo"
cd "${LAB}/repo"
git config user.email t@example.com; git config user.name tester
mkdir -p tools/scripts && cp "${REAPER}" tools/scripts/
[ -f "${RESPONDER}" ] && cp "${RESPONDER}" tools/scripts/
printf 'build/\nbuild-*/\n!build-src/\n' > .gitignore
echo base > file.txt
git add .gitignore file.txt && git commit -qm base
git remote add origin "${LAB}/origin.git"
git push -q origin main
git remote set-head origin main

make_branch() {  # make_branch <name> <merge-into-main?>
    local name="$1" merge="$2"
    git branch -q "${name}" main
    git worktree add -q "${LAB}/${name}" "${name}"
    (cd "${LAB}/${name}" && echo "${name}" > "${name}.txt" && git add "${name}.txt" \
        && git commit -qm "work on ${name}")
    if [ "${merge}" = merge ]; then
        git merge -q --no-ff "${name}" -m "merge ${name}" 2>/dev/null
        git push -q origin main
    fi
    mkdir -p "${LAB}/${name}/build" && dd if=/dev/zero of="${LAB}/${name}/build/obj.o" bs=1k count=64 2>/dev/null
}

make_branch merged-clean merge
make_branch unmerged     no
make_branch dirty        merge
make_branch live         merge
make_branch tracked-art  merge
git fetch -q origin main

# Age every build dir past the idle window so IDLE is never the reason a case
# is refused — otherwise a pass could be the idle gate rather than the gate
# under test.
find "${LAB}" -type d -name 'build*' -exec touch -t 202001010000 {} \; 2>/dev/null
find "${LAB}" -path '*/build*/*' -exec touch -t 202001010000 {} \; 2>/dev/null

echo
echo "== POSITIVE CONTROL — a merged, clean, idle, quiet worktree must be reapable"
echo "   (without this, every refusal below could be a broken instrument)"
expect "merged-clean/build" "$(verdict merged-clean build)" "REAPABLE"

echo
echo "== GATE: unmerged head must be refused"
expect "unmerged/build (has a commit not on main)" "$(verdict unmerged build)" "unproven history"
echo "   control — merge it, then the SAME directory becomes reapable:"
git merge -q --no-ff unmerged -m "merge unmerged" && git push -q origin main && git fetch -q origin main
expect "unmerged/build (now merged)" "$(verdict unmerged build)" "REAPABLE"

echo
echo "== GATE: dirty worktree must be refused"
echo "modified" >> "${LAB}/dirty/file.txt"
expect "dirty/build (uncommitted tracked change)" "$(verdict dirty build)" "uncommitted work"
echo "   control — revert the edit, then the SAME directory becomes reapable:"
(cd "${LAB}/dirty" && git checkout -q -- file.txt)
expect "dirty/build (clean again)" "$(verdict dirty build)" "REAPABLE"

echo
echo "== GATE: a live process must be refused — via an OPEN FILE HANDLE,"
echo "   not a path substring. The holder's argv does not contain the path."
mkdir -p "${LAB}/live/build"
( cd "${LAB}/live/build" && exec 9> .busy && exec sleep 120 ) &
LIVE_PID=$!
sleep 1
find "${LAB}/live" -name 'build*' -exec touch -t 202001010000 {} \; 2>/dev/null
find "${LAB}/live" -path '*/build*/*' -exec touch -t 202001010000 {} \; 2>/dev/null
echo "   holder argv (no worktree path in it): $(ps -p "${LIVE_PID}" -o args= 2>/dev/null)"
echo "   lsof sees it:                          $(lsof -p "${LIVE_PID}" -Fn 2>/dev/null | grep -c "${LAB}/live") path(s) under the worktree"
expect "live/build (open fd, argv clean)" "$(verdict live build)" "live process"
echo "   control — release the handle, then the SAME directory becomes reapable:"
kill "${LIVE_PID}" 2>/dev/null; wait "${LIVE_PID}" 2>/dev/null; LIVE_PID=""
sleep 1
find "${LAB}/live" -name 'build*' -exec touch -t 202001010000 {} \; 2>/dev/null
find "${LAB}/live" -path '*/build*/*' -exec touch -t 202001010000 {} \; 2>/dev/null
expect "live/build (handle released)" "$(verdict live build)" "REAPABLE"

echo
echo "== GATE: a build-* directory git does NOT report as ignored must be refused"
echo "   (the per-item 'this is regenerable' evidence, not a naming convention)"
mkdir -p "${LAB}/tracked-art/build-src"
echo "source" > "${LAB}/tracked-art/build-src/real.c"
(cd "${LAB}/tracked-art" && git add -f build-src/real.c && git commit -qm "tracked build-src")
touch -t 202001010000 "${LAB}/tracked-art/build-src" "${LAB}/tracked-art/build-src/real.c"
git merge -q --no-ff tracked-art -m "merge tracked-art" 2>/dev/null && git push -q origin main && git fetch -q origin main
echo "   git check-ignore on it: $(cd "${LAB}/tracked-art" && git check-ignore -q -- build-src && echo 'ignored' || echo 'NOT ignored')"
expect "tracked-art/build-src (tracked source)" "$(verdict tracked-art build-src)" "structural check"
echo "   control — an ignored sibling in the same worktree IS reapable:"
mkdir -p "${LAB}/tracked-art/build-cov" && dd if=/dev/zero of="${LAB}/tracked-art/build-cov/o.o" bs=1k count=8 2>/dev/null
touch -t 202001010000 "${LAB}/tracked-art/build-cov" "${LAB}/tracked-art/build-cov/o.o"
expect "tracked-art/build-cov (ignored artifact)" "$(verdict tracked-art build-cov)" "REAPABLE"

echo
echo "== RESPONDER: a refusal record is required, and must be fresh"
if [ -x "${RESPONDER}" ]; then
    EMPTY="${LAB}/receipts-empty"; mkdir -p "${EMPTY}"
    "${RESPONDER}" --receipt-dir "${EMPTY}" >/dev/null 2>&1; rc=$?
    [ "${rc}" -eq 2 ] && ok "no refusal record -> exit 2 (declines to act)" \
                      || bad "no refusal record -> exit ${rc}, wanted 2"

    STALE="${LAB}/receipts-stale"; mkdir -p "${STALE}"
    old_stamp="$(/usr/bin/python3 -c 'import datetime as d;print((d.datetime.now(d.timezone.utc)-d.timedelta(hours=6)).isoformat().replace("+00:00","Z"))')"
    cat > "${STALE}/lane.json" <<JSON
{"schema_version":1,"kind":"tartci.disk-admission","status":"denied",
 "reason":"disk_capacity_insufficient","observed_at":"${old_stamp}",
 "free_bytes":15341128581,"floor_bytes":26843545600,"required_bytes":26843545600,
 "probe_path":"${LAB}","runner":"test-01","lane":"test"}
JSON
    "${RESPONDER}" --receipt-dir "${STALE}" >/dev/null 2>&1; rc=$?
    [ "${rc}" -eq 2 ] && ok "6h-old refusal -> exit 2 (a stale shortfall is not authority)" \
                      || bad "stale refusal -> exit ${rc}, wanted 2"

    OTHER="${LAB}/receipts-other"; mkdir -p "${OTHER}"
    fresh_stamp="$(/usr/bin/python3 -c 'import datetime as d;print(d.datetime.now(d.timezone.utc).isoformat().replace("+00:00","Z"))')"
    cat > "${OTHER}/lane.json" <<JSON
{"schema_version":1,"kind":"tartci.disk-admission","status":"denied",
 "reason":"non_disk_denial","observed_at":"${fresh_stamp}",
 "free_bytes":15341128581,"floor_bytes":26843545600,
 "probe_path":"${LAB}","runner":"test-01","lane":"test"}
JSON
    "${RESPONDER}" --receipt-dir "${OTHER}" >/dev/null 2>&1; rc=$?
    [ "${rc}" -eq 2 ] && ok "fresh NON-disk refusal -> exit 2 (only a disk-axis denial is authority)" \
                      || bad "non-disk refusal -> exit ${rc}, wanted 2"

    echo "   control — the SAME record, freshly stamped, IS consumed:"
    FRESH="${LAB}/receipts-fresh"; mkdir -p "${FRESH}"
    new_stamp="$(/usr/bin/python3 -c 'import datetime as d;print(d.datetime.now(d.timezone.utc).isoformat().replace("+00:00","Z"))')"
    sed "s/${old_stamp}/${new_stamp}/" "${STALE}/lane.json" > "${FRESH}/lane.json"
    out="$("${RESPONDER}" --receipt-dir "${FRESH}" 2>&1)"; rc=$?
    case "${out}" in
        *"consuming refusal"*) ok "fresh refusal -> consumed (exit ${rc})" ;;
        *) bad "fresh refusal was not consumed: ${out}" ;;
    esac

    echo "   control — a host already above the target does nothing:"
    out="$("${RESPONDER}" --free-bytes 999999999999 --floor-bytes 1 --probe-path "${LAB}" 2>&1)"; rc=$?
    case "${rc}:${out}" in
        0:*"Nothing removed"*) ok "already above target -> exit 0, no action" ;;
        *) bad "above-target case -> exit ${rc}: ${out}" ;;
    esac
else
    echo "  (responder not present; skipped — a skip is not a pass)"
fi

echo
echo "== summary: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
