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

GOLDEN=9005
CLONE_BASE=200            # three pool slots, well clear of persistent VMs 101/102
CLONE_MAX=202
GUEST_IPV4_PREFIX=192.168.86
GUEST_IPV4_FIRST_OCTET=251
GUEST_IPV4_GATEWAY=192.168.86.1
CORES=4
MEM_MB=8192
REPO="Generous-Corp/pulp"
ORG="${REPO%%/*}"
LABELS="self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro"
PAT_FILE=/root/.config/pulp/secrets/gh-runner-pat
GOVERNOR=/usr/local/sbin/macpro-governor.sh
RUNNER_GROUP_ID="${PULP_LINUX_RUNNER_GROUP_ID:-}"
GROUP_VERIFIER="${PULP_LINUX_GROUP_VERIFIER:-/usr/local/lib/pulp/verify_linux_runner_group.py}"
GH_CLI="${PULP_LINUX_GH_CLI:-gh}"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { log "ERROR: $*"; exit 1; }

[ -r "$PAT_FILE" ] || die "no PAT at $PAT_FILE"
PAT="$(cat "$PAT_FILE")"

# Repository runners remain dispatch-only. Automatic PR or merge-group work is
# permitted only when the controller can prove that an organization runner
# group admits the protected default-branch workflow and this repository alone.
# The extra label prevents a selector for the restricted pool from matching an
# older repository-level worker during a staged rollout.
REGISTRATION_API="repos/${REPO}"
RUNNER_URL="https://github.com/${REPO}"
RUNNER_GROUP_ARG=""
if [ -n "$RUNNER_GROUP_ID" ]; then
    [[ "$RUNNER_GROUP_ID" =~ ^[0-9]+$ ]] \
        || die "PULP_LINUX_RUNNER_GROUP_ID must be numeric"
    [ "$RUNNER_GROUP_ID" != 1 ] \
        || die "runner group 1 is the default group"
    [ -r "$GROUP_VERIFIER" ] \
        || die "runner-group verifier is missing at $GROUP_VERIFIER"
    command -v "$GH_CLI" >/dev/null 2>&1 \
        || die "$GH_CLI is not on PATH"
    GROUP_NAME="$(GH_TOKEN="$PAT" python3 "$GROUP_VERIFIER" \
        --gh "$GH_CLI" --repo "$REPO" --group-id "$RUNNER_GROUP_ID")" \
        || die "automatic Linux runner group policy is not fail-closed"
    [ -n "$GROUP_NAME" ] || die "runner-group verifier returned an empty name"
    REGISTRATION_API="orgs/${ORG}"
    RUNNER_URL="https://github.com/${ORG}"
    RUNNER_GROUP_ARG="--runnergroup ${GROUP_NAME}"
    LABELS="${LABELS},pulp-auto-linux-x64"
fi

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

# A fresh random MAC per clone consumed a new DHCP lease per job until the LAN
# pool was exhausted on 2026-08-02. Keep both network identities deterministic:
# VMIDs 200..202 map to 192.168.86.251..253 and locally administered MACs.
SLOT_INDEX=$((VMID - CLONE_BASE))
GUEST_IP="${GUEST_IPV4_PREFIX}.$((GUEST_IPV4_FIRST_OCTET + SLOT_INDEX))"
printf -v GUEST_MAC '02:50:55:4c:50:%02x' "$SLOT_INDEX"
# The slot identity is stable for operations and metrics.  The GitHub
# registration name is intentionally unique per boot; the decisions contract
# forbids static registration names because an interrupted runner can leave a
# zombie registration that collides with its replacement.
RUNNER_SLOT_ID="macpro-linux-${VMID}"
RUNNER_NAME="pulp-ci-ephemeral-${VMID}-$(cat /proc/sys/kernel/random/uuid)"

reclaim_stale_slot_runners() {
    [ -n "${PAT:-}" ] || return 0
    local runners_json stale_id stale_name stale_busy
    runners_json="$(curl -fSs -H "Authorization: Bearer $PAT" \
        -H "Accept: application/vnd.github+json" \
        "https://api.github.com/${REGISTRATION_API}/actions/runners?per_page=100" \
        2>/dev/null)" || die "cannot inspect runner registrations for ${RUNNER_SLOT_ID}"
    while IFS=$'\t' read -r stale_id stale_name stale_busy; do
        [ -n "$stale_id" ] || continue
        if [ "$stale_busy" = true ]; then
            log "refusing to reclaim busy stale registration $stale_name"
            continue
        fi
        curl -fSs -o /dev/null -X DELETE \
            -H "Authorization: Bearer $PAT" \
            -H "Accept: application/vnd.github+json" \
            "https://api.github.com/${REGISTRATION_API}/actions/runners/${stale_id}" \
            || die "could not reclaim offline registration $stale_name"
        log "reclaimed offline stale registration $stale_name for ${RUNNER_SLOT_ID}"
    done < <(printf '%s' "$runners_json" | python3 -c '
import json, sys
prefix = sys.argv[1]
data = json.load(sys.stdin)
for runner in data.get("runners", []):
    name = runner.get("name", "")
    if name.startswith(prefix):
        print(f"{runner.get(\"id\", \"\")}\\t{name}\\t{str(runner.get(\"busy\", False)).lower()}")
' "pulp-ci-ephemeral-${VMID}-")
}

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
        runners_json="$(curl -fSs -H "Authorization: Bearer $PAT" \
            -H "Accept: application/vnd.github+json" \
            "https://api.github.com/${REGISTRATION_API}/actions/runners?per_page=100" 2>/dev/null)" \
            || { log "ERROR: cannot read runner registrations; leaving clone $VMID for safe recovery"; return; }
        runner_lookup="$(printf '%s' "$runners_json" | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: sys.exit(1)
for r in d.get('runners',[]):
    if r['name']=='${RUNNER_NAME}': print('found:'+str(r['id'])); break
else: print('not-found:'+str(d.get('total_count', 0)))
" 2>/dev/null)" \
            || { log "ERROR: cannot parse runner registrations; leaving clone $VMID for safe recovery"; return; }
        if [[ "$runner_lookup" == found:* ]]; then
            rid="${runner_lookup#found:}"
            if ! curl -fSs -o /dev/null -X DELETE -H "Authorization: Bearer $PAT" \
                -H "Accept: application/vnd.github+json" \
                "https://api.github.com/${REGISTRATION_API}/actions/runners/${rid}"; then
                log "ERROR: cannot deregister runner id $rid; leaving clone $VMID for safe recovery"
                return
            fi
            log "deregistered runner id $rid"
        elif [ "${runner_lookup#not-found:}" -gt 100 ]; then
            log "ERROR: runner lookup exceeded one API page; leaving clone $VMID for safe recovery"
            return
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
# Claim it for cleanup BEFORE releasing the lock. The guard exists so a failure
# before the clone lands cannot destroy whatever now owns that id — but it is
# read on every exit path, so leaving it unset means nothing is ever destroyed
# and the pool silently becomes persistent runners that leak VMIDs.
CLONED=1

# The id is now committed to disk (200.conf exists), so no other slot can pick
# it. Safe to release before the slow boot/register/run phase.
flock -u 9
qm set "$VMID" --cores "$CORES" --memory "$MEM_MB" --cpulimit "$CORES" \
    --cpuunits 50 --balloon 0 --onboot 0 \
    --net0 "virtio=${GUEST_MAC},bridge=vmbr0" \
    --ipconfig0 "ip=${GUEST_IP}/24,gw=${GUEST_IPV4_GATEWAY}" \
    --nameserver "$GUEST_IPV4_GATEWAY" >/dev/null \
    || die "failed to apply deterministic network identity to clone $VMID"
qm start "$VMID" >/dev/null || die "start failed"

# ── wait for the guest ───────────────────────────────────────────────────────
for _ in $(seq 1 45); do
    qm guest cmd "$VMID" network-get-interfaces 2>/dev/null \
        | grep -Fq "\"ip-address\" : \"${GUEST_IP}\"" && break
    sleep 10
done
qm guest cmd "$VMID" network-get-interfaces 2>/dev/null \
    | grep -Fq "\"ip-address\" : \"${GUEST_IP}\"" \
    || die "clone $VMID never reported expected IP $GUEST_IP"
log "clone $VMID up at $GUEST_IP"
# The golden's host keys were cleared, so each clone presents a NEW key on a
# possibly-recycled DHCP address. Drop any stale entry rather than failing.
ssh-keygen -f /root/.ssh/known_hosts -R "$GUEST_IP" >/dev/null 2>&1 || true

# Reclaim only offline registrations from this stable slot.  Never delete a
# busy runner and never search by a broad repository-wide prefix.
reclaim_stale_slot_runners

for _ in $(seq 1 20); do
    ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=8 \
        "ci@$GUEST_IP" true 2>/dev/null && break
    sleep 10
done

# ── register ephemeral ───────────────────────────────────────────────────────
# Preamble/alias jobs use the GitHub CLI with the per-job GITHUB_TOKEN that
# Actions injects. With token variables cleared, --show-token exposes every
# credential the CLI can resolve from its config or credential store. Refuse a
# clone if the executable is missing or any persistent token is resolvable.
ssh -o BatchMode=yes "ci@$GUEST_IP" '
    command -v gh >/dev/null &&
    ! env -u GH_TOKEN -u GITHUB_TOKEN -u GH_ENTERPRISE_TOKEN \
        -u GITHUB_ENTERPRISE_TOKEN -u GH_HOST \
        gh auth status --show-token 2>&1 \
        | grep -Eq "^[[:space:]-]*Token:"
' || die "golden $GOLDEN lacks an uncredentialed gh CLI"

# A registration token is minted per job and is single-use by design, which is
# what makes --ephemeral viable: the runner takes exactly one job, deregisters
# itself, and the clone is destroyed under it.
log "minting registration token"
RT="$(curl -s -X POST \
    -H "Authorization: Bearer $PAT" -H "Accept: application/vnd.github+json" \
    "https://api.github.com/${REGISTRATION_API}/actions/runners/registration-token" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin).get("token",""))')"
[ -n "$RT" ] || die "could not mint a registration token (PAT scope or expiry?)"

log "registering ephemeral runner ${RUNNER_NAME} (slot ${RUNNER_SLOT_ID}) on $VMID"
ssh -o BatchMode=yes "ci@$GUEST_IP" "
    cd ~/actions-runner
    ./config.sh --unattended --ephemeral --replace \
      --url ${RUNNER_URL} --token ${RT} ${RUNNER_GROUP_ARG} \
      --name ${RUNNER_NAME} --labels ${LABELS} --work _work
" >/dev/null 2>&1 || die "runner registration failed"

# ── run exactly one job ──────────────────────────────────────────────────────
# run.sh exits once the single job completes, because of --ephemeral.
log "waiting for one job (runner exits when done)"
ssh -o BatchMode=yes "ci@$GUEST_IP" 'cd ~/actions-runner && ./run.sh' 2>&1 \
    | sed 's/^/    /'
log "job finished on $VMID"
