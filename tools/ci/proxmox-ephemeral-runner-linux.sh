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
GUEST_IPV4_PREFIX=10.240
AUTOMATIC_GUEST_DNS_SERVER=1.1.1.1
ISOLATED_BRIDGE_PREFIX="${PULP_LINUX_ISOLATED_BRIDGE_PREFIX:-vmbr-ci}"
LEGACY_GUEST_IPV4_PREFIX=192.168.86
LEGACY_GUEST_IPV4_FIRST_OCTET=251
LEGACY_GUEST_IPV4_GATEWAY=192.168.86.1
CORES=4
MEM_MB=8192
REPO="Generous-Corp/pulp"
ORG="${REPO%%/*}"
BASE_LABELS="self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro"
LABELS="${PULP_RUNNER_LABELS:-self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro}"
RUNNER_NAME_PREFIX="${PULP_RUNNER_NAME_PREFIX:-pulp-ci-ephemeral}"
RUNNER_GROUP_POLICY="${PULP_LINUX_RUNNER_GROUP_POLICY:-trusted}"
PAT_FILE=/root/.config/pulp/secrets/gh-runner-pat
ORG_PAT_FILE="${PULP_LINUX_ORG_PAT_FILE:-/root/.config/pulp/secrets/gh-org-runner-pat}"
GITHUB_AUTH_MODE="${PULP_LINUX_GITHUB_AUTH_MODE:-token-file}"
GOVERNOR=/usr/local/sbin/macpro-governor.sh
RUNNER_GROUP_ID="${PULP_LINUX_RUNNER_GROUP_ID:-}"
GROUP_VERIFIER="${PULP_LINUX_GROUP_VERIFIER:-/usr/local/lib/pulp/verify_linux_runner_group.py}"
GH_CLI="${PULP_LINUX_GH_CLI:-gh}"
FIREWALL_STATUS_BIN="${PULP_LINUX_FIREWALL_STATUS_BIN:-pve-firewall}"
FIREWALL_DIR="${PULP_LINUX_FIREWALL_DIR:-/etc/pve/firewall}"
VMID_LOCK=/var/lock/pulp-ephemeral-vmid.lock
AUTOMATIC_NETWORK_ISOLATION=0
GITHUB_API_READY=0
PAT=""
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { log "ERROR: $*"; exit 1; }

credential_file_secure() {
    local path="$1" metadata
    [ -f "$path" ] && [ ! -L "$path" ] && [ -r "$path" ] || return 1
    metadata="$(stat -c '%u:%a' -- "$path" 2>/dev/null)" || return 1
    [ "$metadata" = "0:600" ]
}

app_helper_secure() {
    local path="$1" metadata owner mode mode_value
    [ "$path" = /usr/local/bin/ghapp ] || return 1
    [ -f "$path" ] && [ ! -L "$path" ] && [ -x "$path" ] || return 1
    metadata="$(stat -c '%u:%a' -- "$path" 2>/dev/null)" || return 1
    owner="${metadata%%:*}"
    mode="${metadata#*:}"
    [ "$owner" = 0 ] && [[ "$mode" =~ ^[0-7]{3,4}$ ]] || return 1
    mode_value=$((8#$mode))
    (( (mode_value & 8#022) == 0 ))
}

configure_github_auth() {
    local scope="$1" credential_file="$2"
    command -v "$GH_CLI" >/dev/null 2>&1 || die "$GH_CLI is not on PATH"
    case "$GITHUB_AUTH_MODE" in
        token-file)
            credential_file_secure "$credential_file" \
                || die "$scope runner credential must be a root-owned mode-0600 regular file"
            PAT="$(cat "$credential_file")"
            [ -n "$PAT" ] || die "$scope runner credential is empty"
            ;;
        app-helper)
            [ "$scope" = organization ] \
                || die "GitHub App helper authentication is restricted to organization runners"
            app_helper_secure "$GH_CLI" \
                || die "organization GitHub App helper must be exact root-owned non-writable /usr/local/bin/ghapp"
            PAT=""
            ;;
        *) die "unsupported PULP_LINUX_GITHUB_AUTH_MODE" ;;
    esac
    GITHUB_API_READY=1
}

github_api() {
    [ "$GITHUB_API_READY" = 1 ] || die "GitHub API authentication is not configured"
    if [ "$GITHUB_AUTH_MODE" = token-file ]; then
        GH_TOKEN="$PAT" "$GH_CLI" api "$@"
    else
        env -u GH_TOKEN -u GITHUB_TOKEN -u GH_ENTERPRISE_TOKEN \
            -u GITHUB_ENTERPRISE_TOKEN HOME=/root "$GH_CLI" api "$@"
    fi
}

verify_runner_group() {
    if [ "$GITHUB_AUTH_MODE" = token-file ]; then
        GH_TOKEN="$PAT" python3 "$GROUP_VERIFIER" "$@"
    else
        env -u GH_TOKEN -u GITHUB_TOKEN -u GH_ENTERPRISE_TOKEN \
            -u GITHUB_ENTERPRISE_TOKEN HOME=/root \
            python3 "$GROUP_VERIFIER" "$@"
    fi
}

destroy_clone_and_firewall_policy() {
    local vmid="$1" firewall_file="$2" result=0
    [[ "$vmid" =~ ^20[0-2]$ ]] || return 1
    exec 9>"$VMID_LOCK" || return 1
    flock -w 300 9 || return 1
    # The allocation path holds this same lock while selecting and cloning a
    # free VMID. Keep it through policy removal so a successor cannot claim the
    # destroyed VMID, install its own firewall, and have this predecessor delete
    # that new policy after the successor's active-rule proof.
    if qm destroy "$vmid" --purge >/dev/null; then
        rm -f -- "$firewall_file" || result=1
    else
        result=1
    fi
    flock -u 9 || result=1
    return "$result"
}

deferred_cleanup() {
    local vmid="$1" runner_name="$2" registration_api="$3" legacy_credential="${4:-}"
    local expected_credential deadline runners_tsv runner_lookup rid _ busy status labels
    [[ "$vmid" =~ ^20[0-2]$ ]] || die "invalid deferred-cleanup VMID"
    [[ "$runner_name" =~ ^[A-Za-z0-9._-]+$ ]] || die "invalid deferred-cleanup runner name"
    case "$registration_api" in
        "repos/${REPO}") expected_credential="$PAT_FILE" ;;
        "orgs/${ORG}") expected_credential="$ORG_PAT_FILE" ;;
        *) die "invalid deferred-cleanup registration scope" ;;
    esac
    if [ -n "$legacy_credential" ]; then
        [ "$GITHUB_AUTH_MODE" = token-file ] \
            || die "legacy deferred-cleanup credential is valid only in token-file mode"
        [ "$legacy_credential" = "$expected_credential" ] \
            || die "invalid legacy deferred-cleanup credential path"
    fi
    if [ "$registration_api" = "repos/${REPO}" ]; then
        configure_github_auth repository "$expected_credential"
    else
        configure_github_auth organization "$expected_credential"
    fi
    deadline=$((SECONDS + 4500))
    while [ "$SECONDS" -lt "$deadline" ]; do
        runners_tsv="$(github_api --paginate \
            "${registration_api}/actions/runners?per_page=100" \
            --jq '.runners[] | [.id,.name,.busy,.status] | @tsv')" || {
                sleep 15
                continue
            }
        runner_lookup="$(printf '%s\n' "$runners_tsv" | awk -F '\t' \
            -v name="$runner_name" '$2 == name')"
        [ -n "$runner_lookup" ] || break
        [ "$(printf '%s\n' "$runner_lookup" | wc -l | tr -d ' ')" = 1 ] \
            || die "duplicate deferred-cleanup runner registrations"
        IFS=$'\t' read -r rid _ busy status <<< "$runner_lookup"
        case "$busy" in
            true) sleep 15; continue ;;
            false) ;;
            *) die "invalid deferred-cleanup runner busy state" ;;
        esac
        { [ "$status" = online ] || [ "$status" = offline ]; } \
            || die "invalid deferred-cleanup runner status"
        if [ "$status" = online ]; then
            github_api --method PUT \
                "${registration_api}/actions/runners/${rid}/labels" \
                -f 'labels[]=pulp-shutdown-fenced' >/dev/null \
                || die "cannot fence deferred-cleanup runner"
            for fence_probe in 1 2; do
                fenced_runner="$(github_api \
                    "${registration_api}/actions/runners/${rid}" \
                    --jq '[.name,.busy,.status,([.labels[].name] | join(","))] | @tsv')" \
                    || die "cannot verify deferred-cleanup fence"
                IFS=$'\t' read -r fenced_name busy status labels <<< "$fenced_runner"
                [ "$fenced_name" = "$runner_name" ] \
                    || die "deferred-cleanup fence resolved the wrong runner"
                [ "$busy" = false ] \
                    || die "deferred-cleanup runner became busy before dispatch fence"
                { [ "$status" = online ] || [ "$status" = offline ]; } \
                    || die "invalid deferred-cleanup fenced runner status"
                case ",$labels," in
                    *,pulp-auto-linux-x64,*|*,pulp-pr-safe-linux-x64,*|*,pulp-build-linux-x64,*|*,pulp-host-macpro,*)
                        die "routing label survived deferred-cleanup fence"
                        ;;
                esac
                case ",$labels," in
                    *,pulp-shutdown-fenced,*) ;;
                    *) die "shutdown label is missing after deferred-cleanup fence" ;;
                esac
                [ "$fence_probe" = 2 ] || sleep 2
            done
        fi
        break
    done
    [ "$SECONDS" -lt "$deadline" ] \
        || die "deferred cleanup timed out while runner remained busy"

    local vm_status
    timeout 20s qm stop "$vmid" >/dev/null 2>&1 || true
    for _ in $(seq 1 60); do
        vm_status="$(qm status "$vmid" 2>/dev/null)" \
            || die "cannot determine deferred-cleanup clone status"
        [ "$vm_status" = "status: stopped" ] && break
        sleep 2
    done
    [ "$vm_status" = "status: stopped" ] \
        || die "deferred-cleanup clone did not stop"

    runners_tsv="$(github_api --paginate \
        "${registration_api}/actions/runners?per_page=100" \
        --jq '.runners[] | [.id,.name,.busy,.status] | @tsv')" \
        || die "cannot confirm deferred-cleanup runner shutdown"
    runner_lookup="$(printf '%s\n' "$runners_tsv" | awk -F '\t' \
        -v name="$runner_name" '$2 == name')"
    if [ -n "$runner_lookup" ]; then
        [ "$(printf '%s\n' "$runner_lookup" | wc -l | tr -d ' ')" = 1 ] \
            || die "duplicate deferred-cleanup runner registrations"
        IFS=$'\t' read -r rid _ busy status <<< "$runner_lookup"
        [ "$busy" = false ] && [ "$status" = offline ] \
            || die "deferred-cleanup runner did not become idle and offline"
        github_api --method DELETE \
            "${registration_api}/actions/runners/${rid}" \
            || die "cannot deregister deferred-cleanup runner"
    fi
    destroy_clone_and_firewall_policy "$vmid" "${FIREWALL_DIR}/${vmid}.fw" \
        || die "cannot atomically destroy deferred-cleanup clone and firewall policy"
    log "deferred cleanup completed for clone $vmid"
}

if [ "${1:-}" = "--deferred-cleanup" ]; then
    { [ "$#" = 4 ] || [ "$#" = 5 ]; } \
        || die "invalid deferred-cleanup arguments"
    deferred_cleanup "$2" "$3" "$4" "${5:-}"
    exit 0
fi

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
    configure_github_auth organization "$ORG_PAT_FILE"
    GROUP_NAME="$(verify_runner_group \
        --gh "$GH_CLI" --repo "$REPO" --group-id "$RUNNER_GROUP_ID" \
        --policy "$RUNNER_GROUP_POLICY")" \
        || die "automatic Linux runner group policy is not fail-closed"
    [ -n "$GROUP_NAME" ] || die "runner-group verifier returned an empty name"
    command -v "$FIREWALL_STATUS_BIN" >/dev/null 2>&1 \
        || die "$FIREWALL_STATUS_BIN is not on PATH"
    for tool in timeout ip iptables-save ip6tables-save ipset ebtables-save; do
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
    case "$RUNNER_GROUP_POLICY" in
        trusted)
            [ "$LABELS" = "$BASE_LABELS" ] \
                && LABELS="${BASE_LABELS},pulp-auto-linux-x64"
            EXPECTED_LABELS="${BASE_LABELS},pulp-auto-linux-x64"
            EXPECTED_PREFIX="pulp-ci-ephemeral"
            ;;
        pr-safe)
            EXPECTED_LABELS="${BASE_LABELS},pulp-pr-safe-linux-x64"
            EXPECTED_PREFIX="pulp-pr-safe-ephemeral"
            ;;
        *) die "unsupported automatic Linux runner-group policy" ;;
    esac
    [ "$LABELS" = "$EXPECTED_LABELS" ] \
        || die "$RUNNER_GROUP_POLICY runner labels do not match the exact policy"
    [ "$RUNNER_NAME_PREFIX" = "$EXPECTED_PREFIX" ] \
        || die "$RUNNER_GROUP_POLICY runner name prefix does not match the exact policy"
    AUTOMATIC_NETWORK_ISOLATION=1
elif [[ ",$LABELS," == *,pulp-auto-linux-x64,* \
    || ",$LABELS," == *,pulp-pr-safe-linux-x64,* ]]; then
    die "automatic Linux capability labels require a verified organization runner group"
else
    configure_github_auth repository "$PAT_FILE"
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
LOCK="$VMID_LOCK"
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
if [ "$AUTOMATIC_NETWORK_ISOLATION" = 1 ]; then
    NETWORK_BRIDGE="${ISOLATED_BRIDGE_PREFIX}${VMID}"
    GUEST_IP="${GUEST_IPV4_PREFIX}.${VMID}.2"
    GUEST_IPV4_GATEWAY="${GUEST_IPV4_PREFIX}.${VMID}.1"
    GUEST_IPV4_PREFIX_LENGTH=30
    GUEST_DNS_SERVER="$AUTOMATIC_GUEST_DNS_SERVER"
else
    NETWORK_BRIDGE=vmbr0
    GUEST_IP="${LEGACY_GUEST_IPV4_PREFIX}.$((LEGACY_GUEST_IPV4_FIRST_OCTET + SLOT_INDEX))"
    GUEST_IPV4_GATEWAY="$LEGACY_GUEST_IPV4_GATEWAY"
    GUEST_IPV4_PREFIX_LENGTH=24
    GUEST_DNS_SERVER="$LEGACY_GUEST_IPV4_GATEWAY"
fi
printf -v GUEST_MAC '02:50:55:4c:50:%02x' "$SLOT_INDEX"
if [ "$AUTOMATIC_NETWORK_ISOLATION" = 1 ]; then
    [ -d "/sys/class/net/${NETWORK_BRIDGE}/bridge" ] \
        || die "automatic runner requires dedicated isolated bridge ${NETWORK_BRIDGE}"
    bridge_ports=("/sys/class/net/${NETWORK_BRIDGE}/brif/"*)
    [ ! -e "${bridge_ports[0]}" ] \
        || die "isolated bridge ${NETWORK_BRIDGE} already has an attached port"
    bridge_ipv4="$(ip -o -4 addr show dev "$NETWORK_BRIDGE" scope global \
        | awk '{ print $4 }')"
    [ "$bridge_ipv4" = "${GUEST_IPV4_GATEWAY}/30" ] \
        || die "isolated bridge ${NETWORK_BRIDGE} must own only ${GUEST_IPV4_GATEWAY}/30"
fi
# The slot identity is stable for operations and metrics.  The GitHub
# registration name is intentionally unique per boot; the decisions contract
# forbids static registration names because an interrupted runner can leave a
# zombie registration that collides with its replacement.
RUNNER_SLOT_ID="${RUNNER_NAME_PREFIX}-${VMID}"
RUNNER_NAME="${RUNNER_SLOT_ID}-$(cat /proc/sys/kernel/random/uuid)"

reclaim_stale_slot_runners() {
    [ "$GITHUB_API_READY" = 1 ] || return 0
    local runners_tsv slot_matches match_count stale_id stale_name stale_busy stale_status
    runners_tsv="$(github_api --paginate \
        "${REGISTRATION_API}/actions/runners?per_page=100" \
        --jq '.runners[] | [.id,.name,.busy,.status] | @tsv')" \
        || die "cannot inspect all runner registrations for ${RUNNER_SLOT_ID}"
    slot_matches="$(printf '%s\n' "$runners_tsv" | awk -F '\t' \
        -v prefix="${RUNNER_SLOT_ID}-" 'index($2, prefix) == 1')"
    [ -n "$slot_matches" ] || return 0
    match_count="$(printf '%s\n' "$slot_matches" | wc -l | tr -d ' ')"
    [ "$match_count" = 1 ] \
        || die "multiple registrations claim ${RUNNER_SLOT_ID}"
    IFS=$'\t' read -r stale_id stale_name stale_busy stale_status <<< "$slot_matches"
    [ "$stale_busy" = false ] \
        || die "registration $stale_name is busy or has an invalid busy state"
    [ "$stale_status" = offline ] \
        || die "registration $stale_name is not offline"
    github_api --method DELETE \
        "${REGISTRATION_API}/actions/runners/${stale_id}" \
        || die "could not reclaim offline registration $stale_name"
    log "reclaimed offline stale registration $stale_name for ${RUNNER_SLOT_ID}"
}

delegate_deferred_cleanup() {
    local cleanup_unit
    cleanup_unit="pulp-ephemeral-cleanup-${VMID}-$(date +%s)"
    if command -v systemd-run >/dev/null 2>&1 \
        && systemd-run --quiet --collect --unit="$cleanup_unit" \
            --service-type=oneshot --property=TimeoutStartSec=80min \
            --property=User=root \
            --property=Restart=on-failure --property=RestartSec=30s \
            --setenv="PULP_LINUX_GITHUB_AUTH_MODE=$GITHUB_AUTH_MODE" \
            --setenv="PULP_LINUX_ORG_PAT_FILE=$ORG_PAT_FILE" \
            --setenv="PULP_LINUX_GH_CLI=$GH_CLI" \
            --setenv="PULP_LINUX_FIREWALL_DIR=$FIREWALL_DIR" \
            "$(readlink -f "$0")" --deferred-cleanup "$VMID" \
            "$RUNNER_NAME" "$REGISTRATION_API"; then
        log "runner is busy; delegated clone $VMID to $cleanup_unit"
        return 0
    fi
    log "ERROR: runner is busy and deferred cleanup could not be scheduled; leaving clone $VMID for safe recovery"
    return 1
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
    # dispatch by replacing every custom routing label with a shutdown fence,
    # then re-read the
    # runner and delete its registration before powering off the clone. If work
    # won the race before the label removal, the second busy read preserves it.
    if [ "$GITHUB_API_READY" = 1 ]; then
        runners_tsv="$(github_api --paginate \
            "${REGISTRATION_API}/actions/runners?per_page=100" \
            --jq '.runners[] | [.id,.name,.busy,.status] | @tsv')" \
            || { log "ERROR: cannot read all runner registrations; leaving clone $VMID for safe recovery"; return; }
        runner_lookup="$(printf '%s\n' "$runners_tsv" | awk -F '\t' \
            -v name="$RUNNER_NAME" '$2 == name')"
        if [ -n "$runner_lookup" ]; then
            [ "$(printf '%s\n' "$runner_lookup" | wc -l | tr -d ' ')" = 1 ] \
                || { log "ERROR: duplicate exact runner registrations; leaving clone $VMID for safe recovery"; return; }
            IFS=$'\t' read -r rid _ runner_busy runner_status <<< "$runner_lookup"
            if [ "$runner_busy" = true ]; then
                delegate_deferred_cleanup || true
                return
            fi
            [ "$runner_busy" = false ] \
                || { log "ERROR: exact runner has invalid busy state; leaving clone $VMID for safe recovery"; return; }
            { [ "$runner_status" = online ] || [ "$runner_status" = offline ]; } \
                || { log "ERROR: exact runner has invalid status; leaving clone $VMID for safe recovery"; return; }
            if [ "$runner_status" = online ]; then
                github_api --method PUT \
                    "${REGISTRATION_API}/actions/runners/${rid}/labels" \
                    -f 'labels[]=pulp-shutdown-fenced' >/dev/null \
                    || { log "ERROR: cannot fence runner dispatch; leaving clone $VMID for safe recovery"; return; }
                for fence_probe in 1 2; do
                    fenced_runner="$(github_api \
                        "${REGISTRATION_API}/actions/runners/${rid}" \
                        --jq '[.name,.busy,.status,([.labels[].name] | join(","))] | @tsv')" \
                        || { log "ERROR: cannot verify dispatch fence; leaving clone $VMID for safe recovery"; return; }
                    IFS=$'\t' read -r fenced_name runner_busy runner_status runner_labels <<< "$fenced_runner"
                    [ "$fenced_name" = "$RUNNER_NAME" ] \
                        || { log "ERROR: dispatch fence resolved the wrong runner; leaving clone $VMID for safe recovery"; return; }
                    if [ "$runner_busy" = true ]; then
                        delegate_deferred_cleanup || true
                        return
                    fi
                    [ "$runner_busy" = false ] \
                        || { log "ERROR: exact runner has invalid fenced busy state; leaving clone $VMID for safe recovery"; return; }
                    { [ "$runner_status" = online ] || [ "$runner_status" = offline ]; } \
                        || { log "ERROR: exact runner has invalid fenced status; leaving clone $VMID for safe recovery"; return; }
                    case ",$runner_labels," in
                        *,pulp-auto-linux-x64,*|*,pulp-pr-safe-linux-x64,*|*,pulp-build-linux-x64,*|*,pulp-host-macpro,*)
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
                log "fenced dispatch for idle runner id $rid"
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
                    runners_tsv="$(github_api --paginate \
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
            elif ! github_api --method DELETE \
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
    if destroy_clone_and_firewall_policy \
        "$VMID" "${VM_FIREWALL_FILE:-${FIREWALL_DIR}/${VMID}.fw}"; then
        :
    else
        log "WARN: atomic destroy/policy cleanup of $VMID failed — check manually"
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
NET0="virtio=${GUEST_MAC},bridge=${NETWORK_BRIDGE}"
if [ "$AUTOMATIC_NETWORK_ISOLATION" = 1 ]; then
    CONTROLLER_IPV4="$(ip -4 route get "$GUEST_IP" 2>/dev/null \
        | awk '{ for (i = 1; i <= NF; i++) if ($i == "src") { print $(i + 1); exit } }')"
    [[ "$CONTROLLER_IPV4" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] \
        || die "cannot resolve the Proxmox controller IPv4 address for guest ingress"
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
policy_in: DROP
policy_out: ACCEPT

[IPSET ipfilter-net0]
${GUEST_IP}

[RULES]
IN ACCEPT -source ${CONTROLLER_IPV4} -p tcp -dport 22
OUT ACCEPT -dest ${GUEST_DNS_SERVER} -p udp -dport 53
OUT ACCEPT -dest ${GUEST_DNS_SERVER} -p tcp -dport 53
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
    --ipconfig0 "ip=${GUEST_IP}/${GUEST_IPV4_PREFIX_LENGTH},gw=${GUEST_IPV4_GATEWAY}" \
    --nameserver "$GUEST_DNS_SERVER" >/dev/null \
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
        vm_in_rules="$(grep -F -- "-A tap${VMID}i0-IN " <<< "$iptables_rules" || true)"
        vm6_out_rules="$(grep -F -- "-A tap${VMID}i0-OUT " <<< "$ip6tables_rules" || true)"
        vm6_in_rules="$(grep -F -- "-A tap${VMID}i0-IN " <<< "$ip6tables_rules" || true)"
        vm_arp_rules="$(grep -F -- "-A tap${VMID}i0-OUT-ARP " <<< "$ebtables_rules" || true)"
        vm_l2_rules="$(grep -F -- "-A tap${VMID}i0-OUT-PROTO " <<< "$ebtables_rules" || true)"
        bridge_ports=("/sys/class/net/${NETWORK_BRIDGE}/brif/"*)
        isolated_attachment=0
        if [ "${#bridge_ports[@]}" = 1 ] \
            && [ "${bridge_ports[0]##*/}" = "fwpr${VMID}p0" ]; then
            isolated_attachment=1
        fi
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
            && [ "$isolated_attachment" = 1 ] \
            && grep -F -- "-s ${CONTROLLER_IPV4}/32" <<< "$vm_in_rules" \
                | grep -F -- "-p tcp" \
                | grep -F -- "--dport 22" \
                | grep -Fq -- "-j ACCEPT" \
            && grep -Fxq -- "-A tap${VMID}i0-IN -j DROP" <<< "$vm_in_rules" \
            && grep -Fxq -- "-A tap${VMID}i0-IN -j DROP" <<< "$vm6_in_rules" \
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
# Keep the runner-management credential out of process arguments. gh reads it
# from the child environment and prints only the single-use registration token.
RT="$(github_api --method POST \
    "${REGISTRATION_API}/actions/runners/registration-token" --jq .token)"
[ -n "$RT" ] || die "could not mint a registration token (GitHub App/PAT permission or expiry?)"

log "registering ephemeral runner ${RUNNER_NAME} (slot ${RUNNER_SLOT_ID}) on $VMID"
# CommandSettings consumes and removes ACTIONS_RUNNER_INPUT_TOKEN before runner
# configuration. Feed it over SSH stdin so neither the local ssh process nor
# the remote config.sh / Runner.Listener argv exposes the organization token.
printf '%s\n' "$RT" | ssh -o BatchMode=yes "ci@$GUEST_IP" "
    IFS= read -r ACTIONS_RUNNER_INPUT_TOKEN
    export ACTIONS_RUNNER_INPUT_TOKEN
    cd ~/actions-runner
    ./config.sh --unattended --ephemeral --replace \
      --url ${RUNNER_URL} ${RUNNER_GROUP_ARG} \
      --name ${RUNNER_NAME} --labels ${LABELS} --work _work
" >/dev/null 2>&1 || die "runner registration failed"

# ── run exactly one job ──────────────────────────────────────────────────────
# run.sh exits once the single job completes, because of --ephemeral.
log "waiting for one job (runner exits when done)"
ssh -o BatchMode=yes "ci@$GUEST_IP" 'cd ~/actions-runner && ./run.sh' 2>&1 \
    | sed 's/^/    /'
log "job finished on $VMID"
