#!/usr/bin/env bash
# Behavioral test for tools/ci/governed-build.sh — the shipyard local-backend
# build wrapper that acquires a tartci host lease when available and always
# bounds parallelism.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WRAPPER="$ROOT/tools/ci/governed-build.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# The build command records the parallelism the wrapper handed it.
probe="$tmp/probe.sh"
cat >"$probe" <<'SH'
#!/usr/bin/env bash
echo "jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-unset} ctest=${CTEST_PARALLEL_LEVEL:-unset}"
SH
chmod +x "$probe"

# --- 1. No tartci → bounded local parallelism, command still runs ------------
out="$(PULP_TARTCI_LEASES=0 "$WRAPPER" "$probe")"
echo "$out" | grep -qE 'jobs=[0-9]+' || fail "no-tartci: parallelism not bounded ($out)"
jobs="$(echo "$out" | sed -E 's/.*jobs=([0-9]+).*/\1/')"
[ "$jobs" -ge 1 ] || fail "no-tartci: jobs < 1 ($out)"
echo "ok: no-tartci bounded at -j$jobs"

# --- 2. tartci present, lease granted → uses the profile job count + releases -
calls="$tmp/calls.log"
stub="$tmp/tartci"
cat >"$stub" <<SH
#!/usr/bin/env bash
echo "\$*" >> "$calls"
case "\${1:-}" in
  host-profile) printf 'PULP_BUILD_JOBS=7\nTARTCI_AGENT_QOS=normal\n'; exit 0 ;;
  leases)
    case "\${2:-}" in
      acquire) echo '{"ok":true}'; exit 0 ;;
      release) echo '{"ok":true}'; exit 0 ;;
    esac ;;
esac
exit 0
SH
chmod +x "$stub"
out="$(PULP_TARTCI_BIN="$stub" "$WRAPPER" "$probe")"
echo "$out" | grep -q 'jobs=7' || fail "lease-granted: expected -j7 from host profile ($out)"
grep -q 'leases acquire' "$calls" || fail "lease-granted: no acquire call"
grep -q 'leases release' "$calls" || fail "lease-granted: no release call (lease leaked)"
echo "ok: lease acquired at -j7 and released"

# --- 3. tartci present, lease DENIED → proceeds leaseless, does not fail ------
denystub="$tmp/tartci-deny"
cat >"$denystub" <<'SH'
#!/usr/bin/env bash
case "${1:-}" in
  host-profile) printf 'PULP_BUILD_JOBS=7\n'; exit 0 ;;
  leases) [ "${2:-}" = "acquire" ] && { echo '{"ok":false,"reason":"capacity_exceeded"}'; exit 75; } ; exit 0 ;;
esac
exit 0
SH
chmod +x "$denystub"
out="$(PULP_TARTCI_BIN="$denystub" "$WRAPPER" "$probe")" || fail "lease-denied: wrapper must not fail the build"
echo "$out" | grep -qE 'jobs=[0-9]+' || fail "lease-denied: not bounded ($out)"
echo "ok: lease denied → leaseless bounded build, build not failed"

echo "PASS: governed-build.sh"
