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
# Timed, because teardown latency is a real regression surface here: the
# refresher sleeps for a whole interval, and bash defers a trapped signal until
# the current FOREGROUND command returns. A refresher written as
# `while sleep N` therefore ignores its own SIGTERM until the interval elapses,
# which silently adds up to one full interval (default 60s) to the exit of
# every governed build. This case runs at the default interval against an
# instant build, so it can only pass if teardown does not block on the sleep.
started="$(date +%s)"
out="$(PULP_TARTCI_BIN="$stub" "$WRAPPER" "$probe")"
elapsed=$(( $(date +%s) - started ))
echo "$out" | grep -q 'jobs=7' || fail "lease-granted: expected -j7 from host profile ($out)"
grep -q 'leases acquire' "$calls" || fail "lease-granted: no acquire call"
grep -q 'leases release' "$calls" || fail "lease-granted: no release call (lease leaked)"
[ "$elapsed" -lt 15 ] \
  || fail "lease-granted: teardown blocked ${elapsed}s — the heartbeat refresher is not interruptible"
echo "ok: lease acquired at -j7, released, teardown in ${elapsed}s"

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

# --- 4. Long build under a granted lease → the lease keeps beating -----------
# The regression this guards: acquisition stamps ONE heartbeat, tartci marks a
# lease stale after TARTCI_LEASE_STALE_SECS (default 300s), and a compile set
# outlives that easily — so `tartci status` reported stale_heartbeat_live_owner
# for a healthy owner. The interval is overridden to keep the test fast; what
# is asserted is that refreshes happen at all, repeat, and name this lease.
hb_calls="$tmp/hb-calls.log"
hbstub="$tmp/tartci-hb"
cat >"$hbstub" <<SH
#!/usr/bin/env bash
echo "\$*" >> "$hb_calls"
case "\${1:-}" in
  host-profile) printf 'PULP_BUILD_JOBS=3\nTARTCI_AGENT_QOS=normal\n'; exit 0 ;;
  leases) echo '{"ok":true}'; exit 0 ;;
esac
exit 0
SH
chmod +x "$hbstub"

# A build long enough to span several heartbeat intervals.
slow="$tmp/slow.sh"
cat >"$slow" <<'SH'
#!/usr/bin/env bash
sleep 2
echo "jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-unset}"
SH
chmod +x "$slow"

out="$(PULP_TARTCI_BIN="$hbstub" PULP_GOVERNED_BUILD_HEARTBEAT_SECS=1 "$WRAPPER" "$slow")"
echo "$out" | grep -q 'jobs=3' || fail "heartbeat: build did not run under the lease ($out)"
beats="$(grep -c 'leases heartbeat' "$hb_calls" || true)"
[ "$beats" -ge 2 ] || fail "heartbeat: expected repeated refreshes, got $beats"
lease_id="$(sed -nE 's/.*leases acquire --id ([^ ]+).*/\1/p' "$hb_calls" | head -1)"
[ -n "$lease_id" ] || fail "heartbeat: could not read the acquired lease id"
grep -q "leases heartbeat --id $lease_id" "$hb_calls" \
  || fail "heartbeat: refreshed some other lease id than $lease_id"
# Order matters: a refresh after release would re-stamp a returned lease.
last="$(grep -nE 'leases (heartbeat|release)' "$hb_calls" | tail -1)"
echo "$last" | grep -q 'leases release' || fail "heartbeat: release was not the final lease call ($last)"
echo "ok: lease heartbeat refreshed ${beats}x during a long build, then released"

# --- 5. The refresher must not outlive the build -----------------------------
# A leaked refresher is the inverse defect: it keeps a dead owner's lease
# looking alive, which no operator can distinguish from a healthy build.
sleep 2
if pgrep -f "leases heartbeat --id $lease_id" >/dev/null 2>&1; then
  fail "heartbeat: refresher survived the build (leaked lease keepalive)"
fi
echo "ok: heartbeat process cleaned up on exit"

# --- 6. NEGATIVE CONTROL: no lease → no heartbeat ----------------------------
# Proves the refresh is bound to holding a lease rather than fired
# unconditionally; a leaseless fallback has nothing to keep alive.
deny_calls="$tmp/deny-calls.log"
denyhb="$tmp/tartci-deny-hb"
cat >"$denyhb" <<SH
#!/usr/bin/env bash
echo "\$*" >> "$deny_calls"
case "\${1:-}" in
  host-profile) printf 'PULP_BUILD_JOBS=3\n'; exit 0 ;;
  leases)
    case "\${2:-}" in
      acquire) echo '{"ok":false,"reason":"capacity_exceeded"}'; exit 75 ;;
    esac
    echo '{"ok":true}'; exit 0 ;;
esac
exit 0
SH
chmod +x "$denyhb"
PULP_TARTCI_BIN="$denyhb" PULP_GOVERNED_BUILD_HEARTBEAT_SECS=1 "$WRAPPER" "$slow" >/dev/null \
  || fail "no-lease: wrapper must not fail the build"
if grep -q 'leases heartbeat' "$deny_calls"; then
  fail "no-lease: heartbeat sent without holding a lease"
fi
echo "ok: no heartbeat when the lease was denied"

echo "PASS: governed-build.sh"
