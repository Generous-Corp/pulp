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

GOLDEN="${TARTCI_PROXMOX_GOLDEN:-${PULP_LINUX_GOLDEN:-9005}}"
CLONE_BASE="${TARTCI_PROXMOX_CLONE_BASE:-200}" # pool slots, clear of persistent VMs
CLONE_MAX="${TARTCI_PROXMOX_CLONE_MAX:-202}"
GUEST_IPV4_PREFIX=192.168.86
GUEST_IPV4_FIRST_OCTET="${TARTCI_PROXMOX_GUEST_IPV4_FIRST_OCTET:-${PULP_PROXMOX_GUEST_IPV4_FIRST_OCTET:-251}}"
GUEST_IPV4_GATEWAY=192.168.86.1
CORES=4
MEM_MB=8192
REPO="${TARTCI_RUNNER_REPO:-${PULP_RUNNER_REPO:-Generous-Corp/pulp}}"
ORG="${REPO%%/*}"
LABELS="${TARTCI_RUNNER_LABELS:-${PULP_RUNNER_LABELS:-self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro}}"
RUNNER_NAME_PREFIX="${TARTCI_RUNNER_NAME_PREFIX:-${PULP_RUNNER_NAME_PREFIX:-pulp-ci-ephemeral}}"
VM_NAME_PREFIX="${TARTCI_PROXMOX_VM_NAME_PREFIX:-${PULP_PROXMOX_VM_NAME_PREFIX:-pulp-ci-ephemeral}}"
PAT_FILE="${TARTCI_RUNNER_PAT_FILE:-${PULP_RUNNER_PAT_FILE:-/root/.config/pulp/secrets/gh-runner-pat}}"
ORG_PAT_FILE="${TARTCI_ORG_RUNNER_PAT_FILE:-${PULP_LINUX_ORG_PAT_FILE:-/root/.config/pulp/secrets/gh-org-runner-pat}}"
GOVERNOR=/usr/local/sbin/macpro-governor.sh
RUNNER_GROUP_ID="${TARTCI_RUNNER_GROUP_ID:-${PULP_LINUX_RUNNER_GROUP_ID:-}}"
RUNNER_GROUP_PROFILE="${TARTCI_RUNNER_GROUP_PROFILE:-${PULP_LINUX_RUNNER_GROUP_PROFILE:-trusted}}"
GROUP_VERIFIER="${TARTCI_RUNNER_GROUP_VERIFIER:-${PULP_LINUX_GROUP_VERIFIER:-/usr/local/lib/pulp/verify_linux_runner_group.py}}"
GH_CLI="${TARTCI_GH_CLI:-${PULP_LINUX_GH_CLI:-gh}}"
FIREWALL_STATUS_BIN="${PULP_LINUX_FIREWALL_STATUS_BIN:-pve-firewall}"
FIREWALL_DIR="${PULP_LINUX_FIREWALL_DIR:-/etc/pve/firewall}"
AUTOMATIC_NETWORK_ISOLATION="${TARTCI_RUNNER_NETWORK_ISOLATION:-0}"
KEEP=0

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { log "ERROR: $*"; exit 1; }

while [ "$#" -gt 0 ]; do
    case "$1" in
        --keep) KEEP=1; shift ;;
        --once) shift ;; # explicit one-job mode; this supervisor is one-shot
        --repo) REPO="${2:?--repo requires OWNER/REPO}"; ORG="${REPO%%/*}"; shift 2 ;;
        --labels) LABELS="${2:?--labels requires a comma-separated label list}"; shift 2 ;;
        --golden) GOLDEN="${2:?--golden requires a template id}"; shift 2 ;;
        --name-prefix) RUNNER_NAME_PREFIX="${2:?--name-prefix requires a prefix}"; shift 2 ;;
        --help|-h) sed -n '2,24p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

case "$REPO" in
    */*/*|/*|.*) die "runner repository must be OWNER/REPO: $REPO" ;;
esac
[ -n "$LABELS" ] || die "runner labels must not be empty"

require_firewall_running() {
    local context="$1" attempts="${2:-1}" actual="empty"
    local attempt
    for attempt in $(seq 1 "$attempts"); do
        actual="$("$FIREWALL_STATUS_BIN" status 2>&1)" || actual="query failed"
        [ "$actual" = "Status: enabled/running" ] && return 0
        [ "$attempt" = "$attempts" ] || sleep 1
    done
    die "$context (reported after ${attempts} attempt(s): ${actual:-empty})"
}

PAT=""
command -v "$GH_CLI" >/dev/null 2>&1 \
    || die "$GH_CLI is not on PATH"

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
    [ -r "$ORG_PAT_FILE" ] \
        || die "automatic Linux runner organization PAT is missing"
    PAT="$(cat "$ORG_PAT_FILE")"
    case "$RUNNER_GROUP_PROFILE" in
        trusted)
            LABELS="self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro,pulp-auto-linux-x64"
            ;;
        pr-safe)
            LABELS="self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro,pulp-pr-safe-linux-x64"
            ;;
        *)
            die "PULP_LINUX_RUNNER_GROUP_PROFILE must be trusted or pr-safe"
            ;;
    esac
    GROUP_NAME="$(GH_TOKEN="$PAT" python3 "$GROUP_VERIFIER" \
        --gh "$GH_CLI" --repo "$REPO" --group-id "$RUNNER_GROUP_ID" \
        --profile "$RUNNER_GROUP_PROFILE")" \
        || die "automatic Linux runner group policy is not fail-closed"
    [ -n "$GROUP_NAME" ] || die "runner-group verifier returned an empty name"
    command -v "$FIREWALL_STATUS_BIN" >/dev/null 2>&1 \
        || die "$FIREWALL_STATUS_BIN is not on PATH"
    for tool in iptables-save ip6tables-save ipset ebtables-save; do
        command -v "$tool" >/dev/null 2>&1 \
            || die "$tool is required for automatic runner firewall proof"
    done
    require_firewall_running \
        "automatic Linux runners require the Proxmox firewall"
    [ -d "$FIREWALL_DIR" ] \
        || die "automatic Linux runner firewall directory is missing"
    REGISTRATION_API="orgs/${ORG}"
    RUNNER_URL="https://github.com/${ORG}"
    RUNNER_GROUP_ARG="--runnergroup ${GROUP_NAME}"
    AUTOMATIC_NETWORK_ISOLATION=1
else
    [ -r "$PAT_FILE" ] || die "no repository runner PAT at $PAT_FILE"
    PAT="$(cat "$PAT_FILE")"
fi
if [ "$AUTOMATIC_NETWORK_ISOLATION" = 1 ]; then
    command -v "$FIREWALL_STATUS_BIN" >/dev/null 2>&1 \
        || die "$FIREWALL_STATUS_BIN is not on PATH"
    for tool in iptables-save ip6tables-save ipset ebtables-save; do
        command -v "$tool" >/dev/null 2>&1 \
            || die "$tool is required for automatic runner firewall proof"
    done
    require_firewall_running \
        "isolated Linux runners require the Proxmox firewall"
    [ -d "$FIREWALL_DIR" ] \
        || die "isolated Linux runner firewall directory is missing"
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

for existing_vmid in $(qm list 2>/dev/null | awk 'NR > 1 {print $1}'); do
    [ "$existing_vmid" = "$VMID" ] && continue
    existing_ip="$(qm config "$existing_vmid" 2>/dev/null \
        | sed -n 's/^ipconfig0:.*ip=\([^/,]*\).*/\1/p')"
    [ "$existing_ip" = "$GUEST_IP" ] \
        && die "guest IP ${GUEST_IP} is already assigned to VM ${existing_vmid}"
done
# The slot identity and GitHub registration name are stable. Reusing the same
# name per repository/slot makes the fleet auditable and prevents registration
# churn; cleanup reclaims only a confirmed offline registration for this exact
# slot before reuse.
RUNNER_SLOT_ID="macpro-linux-${VMID}"
RUNNER_NAME="${RUNNER_NAME_PREFIX}-${VMID}"
[ "${#RUNNER_NAME}" -le 64 ] \
    || die "runner name exceeds GitHub's 64-character limit: ${RUNNER_NAME}"

reclaim_stale_slot_runners() {
    [ -n "${PAT:-}" ] || return 0
    local runners_tsv slot_matches match_count stale_id stale_name stale_busy stale_status
    runners_tsv="$(GH_TOKEN="$PAT" "$GH_CLI" api --paginate \
        "${REGISTRATION_API}/actions/runners?per_page=100" \
        --jq '.runners[] | [.id,.name,.busy,.status] | @tsv')" \
        || die "cannot inspect all runner registrations for ${RUNNER_SLOT_ID}"
    slot_matches="$(printf '%s\n' "$runners_tsv" | awk -F '\t' \
        -v prefix="${RUNNER_NAME_PREFIX}-${VMID}-" 'index($2, prefix) == 1')"
    [ -n "$slot_matches" ] || return 0
    match_count="$(printf '%s\n' "$slot_matches" | wc -l | tr -d ' ')"
    [ "$match_count" = 1 ] \
        || die "multiple registrations claim ${RUNNER_SLOT_ID}"
    IFS=$'\t' read -r stale_id stale_name stale_busy stale_status <<< "$slot_matches"
    [ "$stale_busy" = false ] \
        || die "registration $stale_name is busy or has an invalid busy state"
    [ "$stale_status" = offline ] \
        || die "registration $stale_name is not offline"
    GH_TOKEN="$PAT" "$GH_CLI" api --method DELETE \
        "${REGISTRATION_API}/actions/runners/${stale_id}" \
        || die "could not reclaim offline registration $stale_name"
    log "reclaimed offline stale registration $stale_name for ${RUNNER_SLOT_ID}"
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
        runners_tsv="$(GH_TOKEN="$PAT" "$GH_CLI" api --paginate \
            "${REGISTRATION_API}/actions/runners?per_page=100" \
            --jq '.runners[] | [.id,.name,.busy,.status] | @tsv')" \
            || { log "ERROR: cannot read all runner registrations; leaving clone $VMID for safe recovery"; return; }
        runner_lookup="$(printf '%s\n' "$runners_tsv" | awk -F '\t' \
            -v name="$RUNNER_NAME" '$2 == name')"
        if [ -n "$runner_lookup" ]; then
            [ "$(printf '%s\n' "$runner_lookup" | wc -l | tr -d ' ')" = 1 ] \
                || { log "ERROR: duplicate exact runner registrations; leaving clone $VMID for safe recovery"; return; }
            IFS=$'\t' read -r rid _ runner_busy runner_status <<< "$runner_lookup"
            [ "$runner_busy" = false ] && [ "$runner_status" = offline ] \
                || { log "ERROR: exact runner is not offline and idle; leaving clone $VMID for safe recovery"; return; }
            if ! GH_TOKEN="$PAT" "$GH_CLI" api --method DELETE \
                "${REGISTRATION_API}/actions/runners/${rid}"; then
                log "ERROR: cannot deregister runner id $rid; leaving clone $VMID for safe recovery"
                return
            fi
            log "deregistered runner id $rid"
        fi
    fi
    log "destroying clone $VMID"
    qm stop "$VMID" >/dev/null 2>&1 || true
    for _ in $(seq 1 24); do [ "$(qm status "$VMID" 2>/dev/null)" = "status: stopped" ] && break; sleep 5; done
    if qm destroy "$VMID" --purge >/dev/null 2>&1; then
        [ -n "${VM_FIREWALL_FILE:-}" ] && rm -f "$VM_FIREWALL_FILE"
    else
        log "WARN: destroy of $VMID failed — check manually"
    fi
    [ -n "${GUEST_IP:-}" ] && ssh-keygen -f /root/.ssh/known_hosts -R "$GUEST_IP" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# ── clone ────────────────────────────────────────────────────────────────────
# Linked clone: near-instant and thin, because the golden's 120G disk is shared
# copy-on-write. A full clone would copy 120G per job and defeat the purpose.
log "linked-cloning golden $GOLDEN -> $VMID"
if ! qm clone "$GOLDEN" "$VMID" --name "${VM_NAME_PREFIX}-${VMID}" >/dev/null 2>&1; then
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
NET0="virtio=${GUEST_MAC},bridge=vmbr0"
if [ "$AUTOMATIC_NETWORK_ISOLATION" = 1 ]; then
    VM_FIREWALL_FILE="${FIREWALL_DIR}/${VMID}.fw"
    VM_FIREWALL_TMP="$(mktemp)" \
        || die "cannot allocate automatic runner firewall policy"
    umask 077
    if ! cat > "$VM_FIREWALL_TMP" <<EOF
[OPTIONS]
enable: 1
ipfilter: 1
policy_in: ACCEPT
policy_out: ACCEPT

[IPSET ipfilter-net0]
${GUEST_IP}

[RULES]
OUT ACCEPT -dest ${GUEST_IPV4_GATEWAY} -p udp -dport 53
OUT ACCEPT -dest ${GUEST_IPV4_GATEWAY} -p tcp -dport 53
OUT DROP -dest 0.0.0.0/8
OUT DROP -dest 10.0.0.0/8
OUT DROP -dest 100.64.0.0/10
OUT DROP -dest 127.0.0.0/8
OUT DROP -dest 169.254.0.0/16
OUT DROP -dest 172.16.0.0/12
OUT DROP -dest 192.0.0.0/24
OUT DROP -dest 192.0.2.0/24
OUT DROP -dest 192.88.99.0/24
OUT DROP -dest 192.168.0.0/16
OUT DROP -dest 198.18.0.0/15
OUT DROP -dest 198.51.100.0/24
OUT DROP -dest 203.0.113.0/24
OUT DROP -dest 224.0.0.0/4
OUT DROP -dest 240.0.0.0/4
OUT DROP -dest ::/0
EOF
    then
        rm -f "$VM_FIREWALL_TMP"
        die "cannot write automatic runner firewall policy"
    fi
    if ! cp "$VM_FIREWALL_TMP" "$VM_FIREWALL_FILE"; then
        rm -f "$VM_FIREWALL_TMP"
        die "cannot install automatic runner firewall policy"
    fi
    rm -f "$VM_FIREWALL_TMP"
    NET0="${NET0},firewall=1"
fi
qm set "$VMID" --cores "$CORES" --memory "$MEM_MB" --cpulimit "$CORES" \
    --cpuunits 50 --balloon 0 --onboot 0 \
    --net0 "$NET0" \
    --ipconfig0 "ip=${GUEST_IP}/24,gw=${GUEST_IPV4_GATEWAY}" \
    --nameserver "$GUEST_IPV4_GATEWAY" >/dev/null \
    || die "failed to apply deterministic network identity to clone $VMID"
if [ "$AUTOMATIC_NETWORK_ISOLATION" = 1 ]; then
    "$FIREWALL_STATUS_BIN" compile >/dev/null \
        || die "automatic runner firewall policy does not compile"
    # Compiling a newly-created per-VM policy can briefly report pending state
    # while the firewall daemon consumes the cluster filesystem update. Bound
    # that convergence window; starting the guest still remains fail-closed.
    require_firewall_running "automatic runner firewall policy is not active" 10
fi
qm start "$VMID" >/dev/null || die "start failed"
if [ "$AUTOMATIC_NETWORK_ISOLATION" = 1 ]; then
    firewall_active=0
    blocked_ipv4=(
        0.0.0.0/8 10.0.0.0/8 100.64.0.0/10 127.0.0.0/8
        169.254.0.0/16 172.16.0.0/12 192.0.0.0/24 192.0.2.0/24
        192.88.99.0/24 192.168.0.0/16 198.18.0.0/15
        198.51.100.0/24 203.0.113.0/24 224.0.0.0/4 240.0.0.0/4
    )
    for _ in $(seq 1 15); do
        iptables_rules="$(iptables-save 2>/dev/null)" || iptables_rules=""
        ip6tables_rules="$(ip6tables-save 2>/dev/null)" || ip6tables_rules=""
        ebtables_rules="$(ebtables-save 2>/dev/null)" || ebtables_rules=""
        vm_out_rules="$(grep -F -- "-A tap${VMID}i0-OUT " <<< "$iptables_rules" || true)"
        vm6_out_rules="$(grep -F -- "-A tap${VMID}i0-OUT " <<< "$ip6tables_rules" || true)"
        vm_arp_rules="$(grep -F -- "-A tap${VMID}i0-OUT-ARP " <<< "$ebtables_rules" || true)"
        all_ipv4_drops=1
        for subnet in "${blocked_ipv4[@]}"; do
            grep -Eq -- "-d ${subnet//./\\.}( .*)? -j DROP" <<< "$vm_out_rules" \
                || { all_ipv4_drops=0; break; }
        done
        if [ "$all_ipv4_drops" = 1 ] \
            && grep -Eq -- "^-A tap${VMID}i0-OUT( -d ::/0)? -j DROP$" <<< "$vm6_out_rules" \
            && grep -Fq -- "--arp-ip-src ${GUEST_IP} -j RETURN" <<< "$vm_arp_rules" \
            && grep -Fq -- "-A tap${VMID}i0-OUT-ARP -j DROP" <<< "$vm_arp_rules" \
            && ipset test "PVEFW-${VMID}-ipfilter-net0-v4" "$GUEST_IP" >/dev/null 2>&1; then
            firewall_active=1
            break
        fi
        sleep 1
    done
    [ "$firewall_active" = 1 ] \
        || die "automatic runner firewall rules are not installed"
fi

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

# Reclaim only offline registrations from this exact slot. Never delete a busy
# runner and never search by a broad repository-wide prefix.
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
TOKEN_FILE=/tmp/tartci-jit-token
ssh -o BatchMode=yes "ci@$GUEST_IP" \
    "umask 077; install -m 600 /dev/stdin ${TOKEN_FILE}" <<<"$RT" \
    || die "could not transfer the short-lived registration token"
unset RT
registration_output="$(ssh -o BatchMode=yes "ci@$GUEST_IP" "
    cd ~/actions-runner
    trap 'rm -f ${TOKEN_FILE}' EXIT
    registration_token=\$(cat ${TOKEN_FILE})
    ./config.sh --unattended --ephemeral --replace \
      --url ${RUNNER_URL} --token \"\${registration_token}\" ${RUNNER_GROUP_ARG} \
      --name ${RUNNER_NAME} --labels ${LABELS} --work _work
")" || {
    printf '%s\n' "$registration_output" | sed 's/[Tt]oken[^[:space:]]*/token=<redacted>/g' >&2
    die "runner registration failed"
}

# ── run exactly one job ──────────────────────────────────────────────────────
# run.sh exits once the single job completes, because of --ephemeral.
log "waiting for one job (runner exits when done)"
ssh -o BatchMode=yes "ci@$GUEST_IP" 'cd ~/actions-runner && ./run.sh' 2>&1 \
    | sed 's/^/    /'
log "job finished on $VMID"
