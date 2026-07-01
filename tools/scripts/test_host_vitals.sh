#!/usr/bin/env bash
# Hermetic tests for host_vitals.sh. Stubs `sysctl` (via PULP_VITALS_SYSCTL) and
# points the report scan at a temp dir (PULP_VITALS_REPORTS_DIR) with files aged
# relative to a fixed PULP_VITALS_NOW, so every health level is exercised with no
# dependence on the real host's memory pressure or crash reports.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VITALS="${SCRIPT_DIR}/host_vitals.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

NOW=1600000000   # fixed "now" for deterministic report ages
PASS=0; FAIL=0

# A stub sysctl driven by STUB_NCPU / STUB_LOAD1 / STUB_PRESSURE.
STUB="${WORK}/sysctl-stub"
cat > "$STUB" <<'EOF'
#!/usr/bin/env bash
# emulate `sysctl -n <key>`
key="${2:-}"
case "$key" in
  hw.ncpu) echo "${STUB_NCPU:-28}" ;;
  vm.loadavg) echo "{ ${STUB_LOAD1:-1.00} 1.00 1.00 }" ;;
  kern.memorystatus_vm_pressure_level)
    if [ -n "${STUB_PRESSURE:-}" ]; then echo "${STUB_PRESSURE}"; else exit 1; fi ;;
  *) exit 1 ;;
esac
EOF
chmod +x "$STUB"

# make_report <dir> <name> <age_seconds> — create a report file aged age_seconds
# before NOW by setting its mtime with touch -t.
make_report() {
  local dir="$1" name="$2" age="$3" target fmt
  mkdir -p "$dir"
  : > "${dir}/${name}"
  target=$(( NOW - age ))
  fmt="$(date -r "$target" +%Y%m%d%H%M.%S 2>/dev/null)"
  touch -t "$fmt" "${dir}/${name}"
}

# run_case <name> <expected_level> <expected_code> [extra args to host_vitals]
# Reads STUB_* and a reports dir from the environment/globals set by the caller.
run_case() {
  local name="$1" exp_level="$2" exp_code="$3"; shift 3
  local out code
  out="$(PULP_VITALS_SYSCTL="$STUB" \
        PULP_VITALS_REPORTS_DIR="${REPORTS_DIR:-$WORK/empty}" \
        PULP_VITALS_NOW="$NOW" \
        PULP_VITALS_UNAME="${UNAME:-Darwin}" \
        STUB_NCPU="${STUB_NCPU:-28}" \
        STUB_LOAD1="${STUB_LOAD1:-1.00}" \
        STUB_PRESSURE="${STUB_PRESSURE:-}" \
        bash "$VITALS" --level "$@" 2>/dev/null)"
  code=$?
  if [ "$out" = "$exp_level" ] && [ "$code" = "$exp_code" ]; then
    PASS=$((PASS+1)); printf '  [PASS] %s (%s/%s)\n' "$name" "$out" "$code"
  else
    FAIL=$((FAIL+1)); printf '  [FAIL] %s: got %s/%s want %s/%s\n' "$name" "$out" "$code" "$exp_level" "$exp_code"
  fi
}

mkdir -p "$WORK/empty"

echo "== host_vitals.sh levels =="

# 1. healthy: normal pressure, low load, no reports
REPORTS_DIR="$WORK/empty" STUB_PRESSURE=1 STUB_LOAD1="5.00" \
  run_case "green: normal pressure + low load" green 0

# 2. warn on pressure level 2
REPORTS_DIR="$WORK/empty" STUB_PRESSURE=2 STUB_LOAD1="1.00" \
  run_case "warn: memory pressure warning" warn 10

# 3. critical on pressure level 4
REPORTS_DIR="$WORK/empty" STUB_PRESSURE=4 STUB_LOAD1="1.00" \
  run_case "critical: memory pressure critical" critical 20

# 4. critical on a fresh jetsam (<5min)
D="$WORK/jetsam_fresh"; make_report "$D" "JetsamEvent-2020-01-01-000000.ips" 60
REPORTS_DIR="$D" STUB_PRESSURE=1 STUB_LOAD1="1.00" \
  run_case "critical: jetsam 60s ago" critical 20

# 5. warn on a jetsam in the 5-15min window
D="$WORK/jetsam_recovering"; make_report "$D" "JetsamEvent-2020-01-01-000000.ips" 600
REPORTS_DIR="$D" STUB_PRESSURE=1 STUB_LOAD1="1.00" \
  run_case "warn: jetsam 600s ago" warn 10

# 6. warn on a fresh WindowServer crash (<15min), no jetsam
D="$WORK/winserver"; make_report "$D" "WindowServer-2020-01-01-000000.ips" 300
REPORTS_DIR="$D" STUB_PRESSURE=1 STUB_LOAD1="1.00" \
  run_case "warn: WindowServer crash 300s ago" warn 10

# 7. warn on load > 3x cores (corroborating only)
REPORTS_DIR="$WORK/empty" STUB_PRESSURE=1 STUB_NCPU=28 STUB_LOAD1="100.00" \
  run_case "warn: load 100 > 3x28" warn 10

# 8. an OLD jetsam (>15min) is ignored -> green
D="$WORK/jetsam_old"; make_report "$D" "JetsamEvent-2020-01-01-000000.ips" 2000
REPORTS_DIR="$D" STUB_PRESSURE=1 STUB_LOAD1="5.00" \
  run_case "green: jetsam 2000s ago is ignored" green 0

# 9. a healthy parallel build (load ~1.5x cores, normal pressure) must NOT trip
REPORTS_DIR="$WORK/empty" STUB_PRESSURE=1 STUB_NCPU=28 STUB_LOAD1="42.00" \
  run_case "green: honest build load (1.5x cores) stays green" green 0

# 10. non-macOS: pressure signal ignored, never critical
UNAME=Linux REPORTS_DIR="$WORK/empty" STUB_PRESSURE=4 STUB_LOAD1="1.00" \
  run_case "linux: pressure ignored, never critical" green 0
UNAME=Linux REPORTS_DIR="$WORK/empty" STUB_PRESSURE= STUB_NCPU=8 STUB_LOAD1="50.00" \
  run_case "linux: high load warns" warn 10

# 11. JSON output carries the level + reason for a critical case
json_out="$(PULP_VITALS_SYSCTL="$STUB" PULP_VITALS_REPORTS_DIR="$WORK/empty" \
  PULP_VITALS_NOW="$NOW" PULP_VITALS_UNAME=Darwin STUB_PRESSURE=4 STUB_LOAD1="1.00" \
  bash "$VITALS" --json 2>/dev/null)"
if printf '%s' "$json_out" | grep -q '"level":"critical"' \
   && printf '%s' "$json_out" | grep -q '"code":20'; then
  PASS=$((PASS+1)); printf '  [PASS] json: critical shape\n'
else
  FAIL=$((FAIL+1)); printf '  [FAIL] json: got %s\n' "$json_out"
fi

# 12. JSON carries sampled_at = the epoch used to compute the ages (PULP_VITALS_NOW),
# so a consumer can reconstruct an incident's absolute time as sampled_at - age_s.
if printf '%s' "$json_out" | grep -q "\"sampled_at\":${NOW}"; then
  PASS=$((PASS+1)); printf '  [PASS] json: sampled_at matches now\n'
else
  FAIL=$((FAIL+1)); printf '  [FAIL] json: sampled_at != %s in %s\n' "$NOW" "$json_out"
fi

echo ""
echo "host_vitals: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ] || exit 1
