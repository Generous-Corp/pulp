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
ORG_PAT_FILE="${PULP_LINUX_ORG_PAT_FILE:-/root/.config/pulp/secrets/gh-org-runner-pat}"
GOVERNOR=/usr/local/sbin/macpro-governor.sh
RUNNER_GROUP_ID="${PULP_LINUX_RUNNER_GROUP_ID:-}"
GROUP_VERIFIER="${PULP_LINUX_GROUP_VERIFIER:-/usr/local/lib/pulp/verify_linux_runner_group.py}"
GH_CLI="${PULP_LINUX_GH_CLI:-gh}"
FIREWALL_STATUS_BIN="${PULP_LINUX_FIREWALL_STATUS_BIN:-pve-firewall}"
FIREWALL_DIR="${PULP_LINUX_FIREWALL_DIR:-/etc/pve/firewall}"
AUTOMATIC_NETWORK_ISOLATION=0
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { log "ERROR: $*"; exit 1; }

[ -r "$PAT_FILE" ] || die "no PAT at $PAT_FILE"
PAT="$(cat "$PAT_FILE")"
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
    GROUP_NAME="$(GH_TOKEN="$PAT" python3 "$GROUP_VERIFIER" \
        --gh "$GH_CLI" --repo "$REPO" --group-id "$RUNNER_GROUP_ID")" \
        || die "automatic Linux runner group policy is not fail-closed"
    [ -n "$GROUP_NAME" ] || die "runner-group verifier returned an empty name"
    command -v "$FIREWALL_STATUS_BIN" >/dev/null 2>&1 \
        || die "$FIREWALL_STATUS_BIN is not on PATH"
    for tool in timeout iptables-save ip6tables-save ipset ebtables-save; do
        command -v "$tool" >/dev/null 2>&1 \
            || die "$tool is required for automatic runner firewall proof"
    done
    [ "$($FIREWALL_STATUS_BIN status 2>/dev/null)" = "Status: enabled/running" ] \
        || die "automatic Linux runners require the Proxmox firewall"
    [ -d "$FIREWALL_DIR" ] \
        || die "automatic Linux runner firewall directory is missing"
    REGISTRATION_API="orgs/${ORG}"
    RUNNER_URL="https://github.com/${ORG}"
    RUNNER_GROUP_ARG="--runnergroup ${GROUP_NAME}"
    LABELS="${LABELS},pulp-auto-linux-x64"
    AUTOMATIC_NETWORK_ISOLATION=1
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
    local runners_tsv slot_matches match_count stale_id stale_name stale_busy stale_status
    runners_tsv="$(GH_TOKEN="$PAT" "$GH_CLI" api --paginate \
        "${REGISTRATION_API}/actions/runners?per_page=100" \
        --jq '.runners[] | [.id,.name,.busy,.status] | @tsv')" \
        || die "cannot inspect all runner registrations for ${RUNNER_SLOT_ID}"
    slot_matches="$(printf '%s\n' "$runners_tsv" | awk -F '\t' \
        -v prefix="pulp-ci-ephemeral-${VMID}-" 'index($2, prefix) == 1')"
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
    # A completed --ephemeral job deregisters itself. During an operator stop,
    # an idle runner can still be online when the local ssh session exits. Fence
    # automatic dispatch by removing the exact protected label, then re-read the
    # runner and delete its registration before powering off the clone. If work
    # won the race before the label removal, the second busy read preserves it.
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
            [ "$runner_busy" = false ] \
                || { log "ERROR: exact runner is busy; leaving clone $VMID for safe recovery"; return; }
            { [ "$runner_status" = online ] || [ "$runner_status" = offline ]; } \
                || { log "ERROR: exact runner has invalid status; leaving clone $VMID for safe recovery"; return; }
            if [ "$runner_status" = online ]; then
                [ "$AUTOMATIC_NETWORK_ISOLATION" = 1 ] \
                    || { log "ERROR: online dispatch runner cannot be fenced; leaving clone $VMID for safe recovery"; return; }
                GH_TOKEN="$PAT" "$GH_CLI" api --method PUT \
                    "${REGISTRATION_API}/actions/runners/${rid}/labels" \
                    -f 'labels[]=pulp-shutdown-fenced' >/dev/null \
                    || { log "ERROR: cannot fence automatic dispatch; leaving clone $VMID for safe recovery"; return; }
                for fence_probe in 1 2; do
                    fenced_runner="$(GH_TOKEN="$PAT" "$GH_CLI" api \
                        "${REGISTRATION_API}/actions/runners/${rid}" \
                        --jq '[.name,.busy,.status,([.labels[].name] | join(","))] | @tsv')" \
                        || { log "ERROR: cannot verify dispatch fence; leaving clone $VMID for safe recovery"; return; }
                    IFS=$'\t' read -r fenced_name runner_busy runner_status runner_labels <<< "$fenced_runner"
                    [ "$fenced_name" = "$RUNNER_NAME" ] \
                        || { log "ERROR: dispatch fence resolved the wrong runner; leaving clone $VMID for safe recovery"; return; }
                    [ "$runner_busy" = false ] \
                        || { log "ERROR: exact runner became busy before dispatch fence; leaving clone $VMID for safe recovery"; return; }
                    { [ "$runner_status" = online ] || [ "$runner_status" = offline ]; } \
                        || { log "ERROR: exact runner has invalid fenced status; leaving clone $VMID for safe recovery"; return; }
                    case ",$runner_labels," in
                        *,pulp-auto-linux-x64,*|*,pulp-build-linux-x64,*|*,pulp-host-macpro,*)
                            log "ERROR: a routing label survived dispatch fence; leaving clone $VMID for safe recovery"
                            return
                            ;;
                    esac
                    case ",$runner_labels," in
                        *,pulp-shutdown-fenced,*) ;;
                        *)
                            log "ERROR: shutdown label is missing after dispatch fence; leaving clone $VMID for safe recovery"
                            return
                            ;;
                    esac
                    [ "$fence_probe" = 2 ] || sleep 2
                done
                log "fenced automatic dispatch for idle runner id $rid"
                shutdown_deadline=$((SECONDS + 120))
                log "stopping fenced clone $VMID before deregistration"
                timeout 20s qm stop "$VMID" >/dev/null 2>&1 || true
                while [ "$SECONDS" -lt "$shutdown_deadline" ]; do
                    [ "$(qm status "$VMID" 2>/dev/null)" = "status: stopped" ] && break
                    sleep 2
                done
                [ "$(qm status "$VMID" 2>/dev/null)" = "status: stopped" ] \
                    || { log "ERROR: fenced clone $VMID did not stop; leaving it for safe recovery"; return; }
                while [ "$SECONDS" -lt "$shutdown_deadline" ]; do
                    runners_tsv="$(GH_TOKEN="$PAT" "$GH_CLI" api --paginate \
                        "${REGISTRATION_API}/actions/runners?per_page=100" \
                        --jq '.runners[] | [.id,.name,.busy,.status] | @tsv')" \
                        || { log "ERROR: cannot confirm fenced runner shutdown; leaving clone $VMID for safe recovery"; return; }
                    runner_lookup="$(printf '%s\n' "$runners_tsv" | awk -F '\t' \
                        -v name="$RUNNER_NAME" '$2 == name')"
                    [ -n "$runner_lookup" ] || break
                    [ "$(printf '%s\n' "$runner_lookup" | wc -l | tr -d ' ')" = 1 ] \
                        || { log "ERROR: duplicate exact runner registrations; leaving clone $VMID for safe recovery"; return; }
                    IFS=$'\t' read -r rid _ runner_busy runner_status <<< "$runner_lookup"
                    [ "$runner_busy" = false ] \
                        || { log "ERROR: fenced runner is busy during shutdown; leaving clone $VMID for safe recovery"; return; }
                    [ "$runner_status" = offline ] && break
                    [ "$runner_status" = online ] \
                        || { log "ERROR: fenced runner has invalid shutdown status; leaving clone $VMID for safe recovery"; return; }
                    sleep 2
                done
                [ -z "$runner_lookup" ] || [ "$runner_status" = offline ] \
                    || { log "ERROR: fenced runner stayed online before cleanup deadline; leaving clone $VMID for safe recovery"; return; }
            fi
            [ -n "$runner_lookup" ] || rid=""
            if [ -z "$rid" ]; then
                log "runner deregistered itself during fenced shutdown"
            elif ! GH_TOKEN="$PAT" "$GH_CLI" api --method DELETE \
                "${REGISTRATION_API}/actions/runners/${rid}"; then
                log "ERROR: cannot deregister runner id $rid; leaving clone $VMID for safe recovery"
                return
            else
                log "deregistered runner id $rid"
            fi
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
NET0="virtio=${GUEST_MAC},bridge=vmbr0"
if [ "$AUTOMATIC_NETWORK_ISOLATION" = 1 ]; then
    VM_FIREWALL_FILE="${FIREWALL_DIR}/${VMID}.fw"
    VM_FIREWALL_TMP="$(mktemp)" \
        || die "cannot allocate automatic runner firewall policy"
    umask 077
    # The guest has a static IPv4 identity. Permit only ARP and untagged IPv4
    # at layer 2, so IPv6, VLAN encapsulation, and every other EtherType hit the
    # generated protocol chain's terminal DROP before reaching the flat LAN.
    if ! cat > "$VM_FIREWALL_TMP" <<EOF
[OPTIONS]
enable: 1
ipfilter: 1
layer2_protocols: ARP,IPv4
policy_in: ACCEPT
policy_out: ACCEPT

[IPSET ipfilter-net0]
${GUEST_IP}

[RULES]
OUT ACCEPT -dest ${GUEST_IPV4_GATEWAY} -p udp -dport 53
OUT ACCEPT -dest ${GUEST_IPV4_GATEWAY} -p tcp -dport 53
OUT DROP -dest ::/0
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
    # Installing a new VM policy briefly reports enabled/running with pending
    # changes while pve-firewall applies the compiled rules.  Do not mistake
    # that expected transition for a disabled firewall, but remain bounded and
    # fail closed unless the final active state arrives.
    firewall_policy_active=0
    for _ in $(seq 1 15); do
        if [ "$($FIREWALL_STATUS_BIN status 2>/dev/null)" = "Status: enabled/running" ]; then
            firewall_policy_active=1
            break
        fi
        sleep 1
    done
    [ "$firewall_policy_active" = 1 ] \
        || die "automatic runner firewall policy is not active"
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
        vm_l2_rules="$(grep -F -- "-A tap${VMID}i0-OUT-PROTO " <<< "$ebtables_rules" || true)"
        all_ipv4_drops=1
        for subnet in "${blocked_ipv4[@]}"; do
            grep -Eq -- "-d ${subnet//./\\.}( .*)? -j DROP" <<< "$vm_out_rules" \
                || { all_ipv4_drops=0; break; }
        done
        # ip6tables-save normalizes an all-address destination by omitting
        # "-d ::/0" on this Proxmox release.  Accept either serialization, but
        # still require an unconditional DROP in this VM's outbound chain.
        ipv6_drop_installed=0
        if grep -Eq -- "-d ::/0( .*)? -j DROP" <<< "$vm6_out_rules" \
            || grep -Fxq -- "-A tap${VMID}i0-OUT -j DROP" <<< "$vm6_out_rules"; then
            ipv6_drop_installed=1
        fi
        if [ "$all_ipv4_drops" = 1 ] \
            && [ "$ipv6_drop_installed" = 1 ] \
            && grep -Fq -- "--arp-ip-src ${GUEST_IP} -j RETURN" <<< "$vm_arp_rules" \
            && grep -Fq -- "-A tap${VMID}i0-OUT-ARP -j DROP" <<< "$vm_arp_rules" \
            && grep -Fq -- "-A tap${VMID}i0-OUT -j tap${VMID}i0-OUT-PROTO" <<< "$ebtables_rules" \
            && grep -Fq -- "-A tap${VMID}i0-OUT-PROTO -p ARP -j RETURN" <<< "$vm_l2_rules" \
            && grep -Fq -- "-A tap${VMID}i0-OUT-PROTO -p IPv4 -j RETURN" <<< "$vm_l2_rules" \
            && ! grep -Fq -- "-A tap${VMID}i0-OUT-PROTO -p IPv6 -j RETURN" <<< "$vm_l2_rules" \
            && grep -Fq -- "-A tap${VMID}i0-OUT-PROTO -j DROP" <<< "$vm_l2_rules" \
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
