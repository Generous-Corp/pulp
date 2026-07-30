#!/usr/bin/env bash
# pulp-ephemeral-runner — one disposable CI runner, from golden clone to teardown.
#
# The isolation model the Pulp fleet already uses (see tartci's
# providers/tart-linux): a golden image carries the expensive, immutable state
# (deps, prebuilt Skia, a warm ccache), and every job runs in a throwaway clone.
#
# Why this rather than a persistent runner with a cleanup hook:
#   - Nothing to clean. The clone is destroyed, so no hook can forget a path and
#     no floor heuristic has to guess what is safe to delete.
#   - The useful cache is never at risk. It lives in the golden, which is
#     read-only; a job cannot poison what the next job inherits.
#   - It closes the ODR class outright. build.yml uses `clean: false` on
#     self-hosted, and reused build dirs across branches are what produced the
#     2026-06-07 random-SEGFAULT incident on the macOS runners.
#
# Admission is delegated to macpro-governor.sh, so this cannot oversubscribe the
# host no matter how many copies are launched.
#
#   pulp-ephemeral-runner.sh            # run one job, then destroy the clone
#   pulp-ephemeral-runner.sh --keep     # leave the clone up (debugging a failure)
#
set -uo pipefail

GOLDEN=9002
CLONE_BASE=200            # clones live at 200..219, well clear of 101/102
CLONE_MAX=219
CORES=4
MEM_MB=8192
REPO="Generous-Corp/pulp"
LABELS="self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro"
PAT_FILE=/root/.config/pulp/secrets/gh-runner-pat
GOVERNOR=/usr/local/sbin/macpro-governor.sh
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { log "ERROR: $*"; exit 1; }

[ -r "$PAT_FILE" ] || die "no PAT at $PAT_FILE"
PAT="$(cat "$PAT_FILE")"

# ── admission ────────────────────────────────────────────────────────────────
# Ask before taking. A refused job queues on GitHub, which is recoverable; an
# oversubscribed host that stops responding is not.
if ! "$GOVERNOR" can-start-new "$CORES" "$MEM_MB" >/dev/null 2>&1; then
    log "REFUSED by governor — host has no room for ${CORES}c/${MEM_MB}M"
    "$GOVERNOR" status | sed 's/^/    /'
    exit 75   # EX_TEMPFAIL: caller should retry later, not treat as broken
fi

# ── claim a clone id, then clone, under one lock ──────────────────────────────
# Selection and creation MUST be atomic. Without the lock, two pool slots read
# "200 is free" in the same instant; one clones it and the other's cleanup then
# destroys 200 out from under the winner, whose `qm start` finds no config file.
# Observed, not theoretical.
LOCK=/var/lock/pulp-ephemeral-vmid.lock
exec 9>"$LOCK" || die "cannot open $LOCK"
flock -w 300 9 || die "timed out waiting for the VMID lock"

VMID=""
for id in $(seq "$CLONE_BASE" "$CLONE_MAX"); do
    qm status "$id" >/dev/null 2>&1 || { VMID="$id"; break; }
done
[ -n "$VMID" ] || { flock -u 9; die "no free clone id in ${CLONE_BASE}..${CLONE_MAX}"; }

cleanup() {
    # Guard: only tear down a VM this invocation actually created. Without this,
    # a failure before the clone lands makes cleanup destroy whatever now owns
    # that id — which is exactly how the race above corrupted a sibling slot.
    [ "${CLONED:-0}" = 1 ] || { log "nothing to clean (no clone created)"; return; }
    if [ "$KEEP" = 1 ]; then
        log "--keep set: leaving VM $VMID up for inspection"
        return
    fi
    # Deregister BEFORE destroying. A completed --ephemeral job deregisters
    # itself, but an interrupted run (kill, reboot, governor abort) would
    # otherwise leave an offline ghost runner that GitHub still schedules to —
    # jobs then queue against a VM that no longer exists.
    if [ -n "${PAT:-}" ]; then
        rid="$(curl -s -H "Authorization: Bearer $PAT" \
                 -H "Accept: application/vnd.github+json" \
                 "https://api.github.com/repos/${REPO}/actions/runners" 2>/dev/null \
             | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: sys.exit()
for r in d.get('runners',[]):
    if r['name']=='pulp-ci-ephemeral-${VMID}': print(r['id']); break
" 2>/dev/null)"
        if [ -n "${rid:-}" ]; then
            curl -s -o /dev/null -X DELETE -H "Authorization: Bearer $PAT" \
                -H "Accept: application/vnd.github+json" \
                "https://api.github.com/repos/${REPO}/actions/runners/${rid}" || true
            log "deregistered runner id $rid"
        fi
    fi
    log "destroying clone $VMID"
    qm stop "$VMID" >/dev/null 2>&1 || true
    for _ in $(seq 1 24); do [ "$(qm status "$VMID" 2>/dev/null)" = "status: stopped" ] && break; sleep 5; done
    qm destroy "$VMID" --purge >/dev/null 2>&1 || log "WARN: destroy of $VMID failed — check manually"
    [ -n "${GUEST_IP:-}" ] && ssh-keygen -f /root/.ssh/known_hosts -R "$GUEST_IP" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# ── clone ────────────────────────────────────────────────────────────────────
# Linked clone: near-instant and thin, because the golden's 120G disk is shared
# copy-on-write. A full clone would copy 120G per job and defeat the purpose.
log "linked-cloning golden $GOLDEN -> $VMID"
if ! qm clone "$GOLDEN" "$VMID" --name "pulp-ci-ephemeral-$VMID" >/dev/null 2>&1; then
    flock -u 9
    die "clone failed"
fi
# The id is now committed to disk (200.conf exists), so no other slot can pick
# it. Safe to release before the slow boot/register/run phase.
flock -u 9
qm set "$VMID" --cores "$CORES" --memory "$MEM_MB" --cpulimit "$CORES" \
    --cpuunits 50 --balloon 0 --onboot 0 >/dev/null
qm start "$VMID" >/dev/null || die "start failed"

# ── wait for the guest ───────────────────────────────────────────────────────
GUEST_IP=""
for _ in $(seq 1 45); do
    GUEST_IP="$(qm guest cmd "$VMID" network-get-interfaces 2>/dev/null \
        | grep -oE '192\.168\.[0-9]+\.[0-9]+' | head -1)"
    [ -n "$GUEST_IP" ] && break
    sleep 10
done
[ -n "$GUEST_IP" ] || die "clone $VMID never reported an IP"
log "clone $VMID up at $GUEST_IP"
# The golden's host keys were cleared, so each clone presents a NEW key on a
# possibly-recycled DHCP address. Drop any stale entry rather than failing.
ssh-keygen -f /root/.ssh/known_hosts -R "$GUEST_IP" >/dev/null 2>&1 || true

for _ in $(seq 1 20); do
    ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 \
        "ci@$GUEST_IP" true 2>/dev/null && break
    sleep 10
done

# ── register ephemeral ───────────────────────────────────────────────────────
# A registration token is minted per job and is single-use by design, which is
# what makes --ephemeral viable: the runner takes exactly one job, deregisters
# itself, and the clone is destroyed under it.
log "minting registration token"
RT="$(curl -s -X POST \
    -H "Authorization: Bearer $PAT" -H "Accept: application/vnd.github+json" \
    "https://api.github.com/repos/${REPO}/actions/runners/registration-token" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin).get("token",""))')"
[ -n "$RT" ] || die "could not mint a registration token (PAT scope or expiry?)"

log "registering ephemeral runner on $VMID"
ssh -o BatchMode=yes "ci@$GUEST_IP" "
    cd ~/actions-runner
    ./config.sh --unattended --ephemeral --replace \
      --url https://github.com/${REPO} --token ${RT} \
      --name pulp-ci-ephemeral-${VMID} --labels ${LABELS} --work _work
" >/dev/null 2>&1 || die "runner registration failed"

# ── run exactly one job ──────────────────────────────────────────────────────
# run.sh exits once the single job completes, because of --ephemeral.
log "waiting for one job (runner exits when done)"
ssh -o BatchMode=yes "ci@$GUEST_IP" 'cd ~/actions-runner && ./run.sh' 2>&1 \
    | sed 's/^/    /'
log "job finished on $VMID"
