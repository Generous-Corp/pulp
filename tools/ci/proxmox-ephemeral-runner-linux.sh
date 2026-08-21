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
GUEST_IPV4_PREFIX=10.240
AUTOMATIC_GUEST_DNS_SERVER=1.1.1.1
ISOLATED_BRIDGE_PREFIX="${PULP_LINUX_ISOLATED_BRIDGE_PREFIX:-vmbr-ci}"
LEGACY_GUEST_IPV4_PREFIX=192.168.86
LEGACY_GUEST_IPV4_FIRST_OCTET=251
LEGACY_GUEST_IPV4_GATEWAY=192.168.86.1
CORES=4
MEM_MB=8192
REPO="${TARTCI_RUNNER_REPO:-${PULP_RUNNER_REPO:-Generous-Corp/pulp}}"
ORG="${REPO%%/*}"
BASE_LABELS="self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro"
if [ -n "${TARTCI_RUNNER_LABELS:-}" ]; then
    EFFECTIVE_RUNNER_LABELS="$TARTCI_RUNNER_LABELS"
elif [ -n "${PULP_RUNNER_LABELS:-}" ]; then
    EFFECTIVE_RUNNER_LABELS="$PULP_RUNNER_LABELS"
else
    EFFECTIVE_RUNNER_LABELS="$BASE_LABELS"
fi
LABELS="${EFFECTIVE_RUNNER_LABELS:-self-hosted,Linux,X64,pulp-build-linux-x64,pulp-host-macpro}"
RUNNER_NAME_PREFIX="${TARTCI_RUNNER_NAME_PREFIX:-${PULP_RUNNER_NAME_PREFIX:-pulp-ci-ephemeral}}"
VM_NAME_PREFIX="${TARTCI_PROXMOX_VM_NAME_PREFIX:-pulp-ci-ephemeral}"
RUNNER_GROUP_POLICY="${TARTCI_RUNNER_GROUP_POLICY:-${PULP_LINUX_RUNNER_GROUP_POLICY:-trusted}}"
RUNNER_GROUP_NAME="${TARTCI_RUNNER_GROUP_NAME:-}"
RUNNER_WORKFLOW="${TARTCI_RUNNER_WORKFLOW:-}"
if [ "$REPO" = "Generous-Corp/pulp" ]; then
    PAT_FILE="${TARTCI_RUNNER_PAT_FILE:-${PULP_RUNNER_PAT_FILE:-/root/.config/pulp/secrets/gh-runner-pat}}"
    ORG_PAT_FILE="${TARTCI_ORG_RUNNER_PAT_FILE:-${PULP_LINUX_ORG_PAT_FILE:-/root/.config/pulp/secrets/gh-org-runner-pat}}"
    GITHUB_AUTH_MODE="${TARTCI_RUNNER_GITHUB_AUTH_MODE:-${PULP_LINUX_GITHUB_AUTH_MODE:-token-file}}"
else
    PAT_FILE="${TARTCI_RUNNER_PAT_FILE:-}"
    ORG_PAT_FILE="${TARTCI_ORG_RUNNER_PAT_FILE:-}"
    GITHUB_AUTH_MODE="${TARTCI_RUNNER_GITHUB_AUTH_MODE:-}"
fi
GOVERNOR=/usr/local/sbin/macpro-governor.sh
RUNNER_GROUP_ID="${TARTCI_RUNNER_GROUP_ID:-${PULP_LINUX_RUNNER_GROUP_ID:-}}"
GROUP_VERIFIER="${TARTCI_RUNNER_GROUP_VERIFIER:-${PULP_LINUX_GROUP_VERIFIER:-/usr/local/lib/pulp/verify_linux_runner_group.py}}"
GH_CLI="${TARTCI_GH_CLI:-${PULP_LINUX_GH_CLI:-gh}}"
FIREWALL_STATUS_BIN="${PULP_LINUX_FIREWALL_STATUS_BIN:-pve-firewall}"
FIREWALL_DIR="${PULP_LINUX_FIREWALL_DIR:-/etc/pve/firewall}"
VMID_LOCK=/var/lock/pulp-ephemeral-vmid.lock
RUNNER_LEASE_DIR=/run/pulp-ephemeral-runner
RUNNER_KEEP_DIR=/var/lib/pulp/ephemeral-runner-keep
HOST_NETWORK_LOCK=/var/lock/pulp-ci-host-network.lock
AUTOMATIC_NETWORK_ISOLATION=0
GITHUB_API_READY=0
PAT=""
KEEP=0
RUNNER_READY_TIMEOUT_SECONDS="${TARTCI_RUNNER_READY_TIMEOUT_SECONDS:-120}"
RUNNER_HEARTBEAT_INTERVAL_SECONDS="${TARTCI_RUNNER_HEARTBEAT_INTERVAL_SECONDS:-15}"

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { log "ERROR: $*"; exit 1; }

vmid_in_range() {
    local vmid="$1"
    [[ "$vmid" =~ ^[0-9]+$ ]] \
        && [ "$vmid" -ge "$CLONE_BASE" ] \
        && [ "$vmid" -le "$CLONE_MAX" ]
}

sanitize_runner_output() {
    sed -E \
        -e 's/(token|authorization|credentials|jitconfig|encoded_jit_config)[=:][^[:space:]]+/\1=<redacted>/Ig' \
        -e 's/(A[A-Za-z0-9_-]{20,})/<redacted>/g'
}

routing_label_survives() {
    local observed_labels="$1" configured_label
    local configured_labels=()
    IFS=',' read -r -a configured_labels <<< "$LABELS"
    for configured_label in "${configured_labels[@]}"; do
        case "$configured_label" in
            self-hosted|Linux|X64) continue ;;
        esac
        case ",$observed_labels," in
            *",${configured_label},"*) return 0 ;;
        esac
    done
    return 1
}

monitor_runner_heartbeat() {
    local misses=0 heartbeat_tsv heartbeat_lookup heartbeat_status heartbeat_busy
    local match_count
    while kill -0 "$RUNNER_PID" 2>/dev/null; do
        if ! heartbeat_tsv="$(github_api --paginate \
            "${REGISTRATION_API}/actions/runners?per_page=100" \
            --jq '.runners[] | [.id,.name,.status,.busy] | @tsv' 2>/dev/null)"; then
            sleep "$RUNNER_HEARTBEAT_INTERVAL_SECONDS"
            continue
        fi
        heartbeat_lookup="$(printf '%s\n' "$heartbeat_tsv" | awk -F '\t' \
            -v name="$RUNNER_NAME" '$2 == name')"
        heartbeat_status=""
        heartbeat_busy=""
        if [ -n "$heartbeat_lookup" ]; then
            match_count="$(printf '%s\n' "$heartbeat_lookup" | wc -l | tr -d ' ')"
            if [ "$match_count" != 1 ]; then
                sleep "$RUNNER_HEARTBEAT_INTERVAL_SECONDS"
                continue
            fi
            IFS=$'\t' read -r _ _ heartbeat_status heartbeat_busy <<< "$heartbeat_lookup"
        fi
        if [ "$heartbeat_busy" = true ] || { [ "$heartbeat_status" = online ] && [ "$heartbeat_busy" = false ]; }; then
            misses=0
        elif [ -z "$heartbeat_lookup" ] \
            || { [ "$heartbeat_status" = offline ] && [ "$heartbeat_busy" = false ]; }; then
            misses=$((misses + 1))
            if [ "$misses" -ge 2 ]; then
                printf 'runner=%s status=%s busy=%s\n' \
                    "$RUNNER_NAME" "${heartbeat_status:-missing}" "${heartbeat_busy:-unknown}" \
                    >"$HEARTBEAT_FAILURE_FILE"
                kill "$RUNNER_PID" 2>/dev/null || true
                break
            fi
        fi
        sleep "$RUNNER_HEARTBEAT_INTERVAL_SECONDS"
    done
}

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
    vmid_in_range "$vmid" || return 1
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
    vmid_in_range "$vmid" || die "invalid deferred-cleanup VMID"
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
        if [ "$status" = online ] || [ "$status" = offline ]; then
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
                routing_label_survives "$labels" \
                    && die "routing label survived deferred-cleanup fence"
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
    # Bind cleanup to this exact clone generation. A transient systemd cleanup
    # unit may restart after the old VM has gone; without the allocation lock
    # and generation proof, that retry could stop a successor that reused the
    # numeric VMID.
    exec 9>"$VMID_LOCK" || die "cannot open the deferred-cleanup VMID lock"
    flock -w 300 9 || die "timed out waiting for the deferred-cleanup VMID lock"
    vm_inventory="$(qm list 2>/dev/null)" \
        || die "cannot inspect VM inventory during deferred cleanup"
    if ! awk -v vmid="$vmid" 'NR > 1 && $1 == vmid { found=1 } END { exit !found }' \
        <<<"$vm_inventory"; then
        rm -f -- "${FIREWALL_DIR}/${vmid}.fw" \
            || die "cannot remove already-absent clone firewall policy"
        flock -u 9 || die "cannot release the deferred-cleanup VMID lock"
        log "deferred cleanup found clone $vmid already absent"
        return
    fi
    clone_generation="$(qm config "$vmid" 2>/dev/null \
        | sed -n 's/^description: pulp-runner-generation=\([^;]*\).*/\1/p')" \
        || die "cannot inspect deferred-cleanup clone generation"
    [ "$clone_generation" = "$runner_name" ] \
        || die "deferred-cleanup VMID $vmid belongs to a different clone generation"
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
    qm destroy "$vmid" --purge >/dev/null \
        || die "cannot destroy deferred-cleanup clone"
    rm -f -- "${FIREWALL_DIR}/${vmid}.fw" \
        || die "cannot remove deferred-cleanup firewall policy"
    flock -u 9 || die "cannot release the deferred-cleanup VMID lock"
    log "deferred cleanup completed for clone $vmid"
}

if [ "${1:-}" = "--deferred-cleanup" ]; then
    { [ "$#" = 4 ] || [ "$#" = 5 ]; } \
        || die "invalid deferred-cleanup arguments"
    deferred_cleanup "$2" "$3" "$4" "${5:-}"
    exit 0
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --keep) KEEP=1; shift ;;
        --once) shift ;;
        --help|-h) sed -n '2,24p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

case "$REPO" in
    */*/*|/*|.*|*' '*) die "runner repository must be OWNER/REPO: $REPO" ;;
esac
[[ "$CLONE_BASE" =~ ^[0-9]+$ && "$CLONE_MAX" =~ ^[0-9]+$ ]] \
    || die "clone VMID bounds must be numeric"
[ "$CLONE_BASE" -ge 1 ] && [ "$CLONE_MAX" -le 254 ] \
    && [ "$CLONE_BASE" -le "$CLONE_MAX" ] \
    || die "clone VMID range must be ordered within 1..254"
[[ "$RUNNER_NAME_PREFIX" =~ ^[A-Za-z0-9._-]+$ ]] \
    || die "runner name prefix must be shell-safe"
[[ "$VM_NAME_PREFIX" =~ ^[A-Za-z0-9._-]+$ ]] \
    || die "VM name prefix must be shell-safe"

if [ "$REPO" != "Generous-Corp/pulp" ]; then
    [ -n "${TARTCI_RUNNER_REPO:-}" ] \
        || die "cross-repository use requires TARTCI_RUNNER_REPO"
    [ -n "${TARTCI_RUNNER_GROUP_ID:-}" ] \
        || die "cross-repository automatic routing requires TARTCI_RUNNER_GROUP_ID"
    [ -n "$RUNNER_GROUP_NAME" ] && [ -n "$RUNNER_WORKFLOW" ] \
        || die "cross-repository routing requires an exact group name and workflow"
    [ -n "${TARTCI_RUNNER_LABELS:-}" ] \
        && [ -n "${TARTCI_RUNNER_NAME_PREFIX:-}" ] \
        && [ -n "${TARTCI_PROXMOX_VM_NAME_PREFIX:-}" ] \
        && [ -n "${TARTCI_PROXMOX_GOLDEN:-}" ] \
        && [ -n "${TARTCI_PROXMOX_CLONE_BASE:-}" ] \
        && [ -n "${TARTCI_PROXMOX_CLONE_MAX:-}" ] \
        || die "cross-repository routing requires explicit labels, names, golden, and VMID range"
    [ -n "${TARTCI_RUNNER_GITHUB_AUTH_MODE:-}" ] \
        && [ -n "${TARTCI_ORG_RUNNER_PAT_FILE:-}" ] \
        || die "cross-repository routing requires explicit GitHub authentication and organization credential identity"
    [[ "$RUNNER_WORKFLOW" =~ ^\.github/workflows/[A-Za-z0-9._-]+\.ya?ml$ ]] \
        || die "cross-repository workflow must be an exact workflow path"
    for required_label in self-hosted Linux X64; do
        case ",$LABELS," in
            *",${required_label},"*) ;;
            *) die "cross-repository labels must include self-hosted,Linux,X64" ;;
        esac
    done
    case ",$LABELS," in
        *,pulp-*) die "cross-repository labels must not reuse a Pulp capability label" ;;
    esac
fi

command -v "$GH_CLI" >/dev/null 2>&1 \
    || die "$GH_CLI is not on PATH"

# Repository runners remain dispatch-only. Automatic PR or merge-group work is
# permitted only when the controller can prove that an organization runner
# group admits the protected default-branch workflow and this repository alone.
# The extra label prevents a selector for the restricted pool from matching an
# older repository-level worker during a staged rollout.
REGISTRATION_API="repos/${REPO}"
if [ -n "$RUNNER_GROUP_ID" ]; then
    [[ "$RUNNER_GROUP_ID" =~ ^[0-9]+$ ]] \
        || die "PULP_LINUX_RUNNER_GROUP_ID must be numeric"
    [ "$RUNNER_GROUP_ID" != 1 ] \
        || die "runner group 1 is the default group"
    [ -r "$GROUP_VERIFIER" ] \
        || die "runner-group verifier is missing at $GROUP_VERIFIER"
    configure_github_auth organization "$ORG_PAT_FILE"
    GROUP_VERIFY_ARGS=(--gh "$GH_CLI" --repo "$REPO" --group-id "$RUNNER_GROUP_ID")
    if [ "$REPO" = "Generous-Corp/pulp" ]; then
        GROUP_VERIFY_ARGS+=(--policy "$RUNNER_GROUP_POLICY")
    else
        GROUP_VERIFY_ARGS+=(--group-name "$RUNNER_GROUP_NAME" --workflow "$RUNNER_WORKFLOW")
    fi
    GROUP_NAME="$(verify_runner_group "${GROUP_VERIFY_ARGS[@]}")" \
        || die "automatic Linux runner group policy is not fail-closed"
    [ -n "$GROUP_NAME" ] || die "runner-group verifier returned an empty name"
    command -v "$FIREWALL_STATUS_BIN" >/dev/null 2>&1 \
        || die "$FIREWALL_STATUS_BIN is not on PATH"
    for tool in timeout ip sysctl iptables iptables-save ip6tables-save ipset ebtables-save; do
        command -v "$tool" >/dev/null 2>&1 \
            || die "$tool is required for automatic runner firewall proof"
    done
    [ "$($FIREWALL_STATUS_BIN status 2>/dev/null)" = "Status: enabled/running" ] \
        || die "automatic Linux runners require the Proxmox firewall"
    [ -d "$FIREWALL_DIR" ] \
        || die "automatic Linux runner firewall directory is missing"
    REGISTRATION_API="orgs/${ORG}"
    if [ "$REPO" = "Generous-Corp/pulp" ]; then
        case "$RUNNER_GROUP_POLICY" in
            trusted)
                if [ "$LABELS" = "$BASE_LABELS" ]; then
                    LABELS="${LABELS},pulp-auto-linux-x64"
                fi
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
    fi
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
if [ "$AUTOMATIC_NETWORK_ISOLATION" = 1 ]; then
    # Match the host setup helper's lock order: network first, VMID second.
    # Holding both through attachment makes its empty-bridge rollback proof
    # mutually exclusive with this clone's topology admission.
    exec 8>"$HOST_NETWORK_LOCK" || die "cannot open the host-network lock"
    flock -w 30 8 || die "timed out waiting for the host-network lock"
fi
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
    [ "$(sysctl -n net.ipv4.ip_forward 2>/dev/null)" = 1 ] \
        || die "automatic runner requires IPv4 forwarding"
    nat_rules="$(iptables-save -t nat)" \
        || die "cannot inspect automatic runner NAT policy"
    nat_count="$(grep -Fc -- \
        "-s ${GUEST_IPV4_PREFIX}.${VMID}.0/30 -o vmbr0 -m comment --comment \"pulp-ci-isolation:${NETWORK_BRIDGE}\" -j MASQUERADE" \
        <<<"$nat_rules")" || {
            nat_status=$?
            [ "$nat_status" = 1 ] || die "cannot count automatic runner NAT policy"
        }
    [ "$nat_count" = 1 ] \
        || die "automatic runner requires exactly one source-scoped NAT rule for ${NETWORK_BRIDGE}"
fi
# The slot identity is stable for operations and metrics.  The GitHub
# registration name is intentionally unique per boot; the decisions contract
# forbids static registration names because an interrupted runner can leave a
# zombie registration that collides with its replacement.
RUNNER_SLOT_ID="${RUNNER_NAME_PREFIX}-${VMID}"
RUNNER_NAME="${RUNNER_SLOT_ID}-$(cat /proc/sys/kernel/random/uuid)"
[ "${#RUNNER_NAME}" -le 64 ] \
    || die "generation-unique runner name exceeds GitHub's 64-character limit"

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
            --setenv="TARTCI_RUNNER_REPO=${REPO}" \
            --setenv="TARTCI_RUNNER_GITHUB_AUTH_MODE=${GITHUB_AUTH_MODE}" \
            --setenv="TARTCI_RUNNER_LABELS=${LABELS}" \
            --setenv="TARTCI_RUNNER_PAT_FILE=${PAT_FILE}" \
            --setenv="TARTCI_ORG_RUNNER_PAT_FILE=${ORG_PAT_FILE}" \
            --setenv="TARTCI_GH_CLI=${GH_CLI}" \
            --setenv="TARTCI_PROXMOX_CLONE_BASE=${CLONE_BASE}" \
            --setenv="TARTCI_PROXMOX_CLONE_MAX=${CLONE_MAX}" \
            --setenv="PULP_LINUX_GITHUB_AUTH_MODE=${GITHUB_AUTH_MODE}" \
            --setenv="PULP_LINUX_ORG_PAT_FILE=${ORG_PAT_FILE}" \
            --setenv="PULP_LINUX_GH_CLI=${GH_CLI}" \
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
    [ -z "${HEARTBEAT_PID:-}" ] || kill "$HEARTBEAT_PID" 2>/dev/null || true
    rm -f -- "${JIT_REQUEST_FILE:-}" "${JIT_CONFIG_FILE:-}" \
        "${RUNNER_OUTPUT_FILE:-}" "${HEARTBEAT_FAILURE_FILE:-}"
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
            if [ "$runner_status" = online ] || [ "$runner_status" = offline ]; then
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
                    if routing_label_survives "$runner_labels"; then
                        log "ERROR: a routing label survived dispatch fence; leaving clone $VMID for safe recovery"
                        return
                    fi
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
remove_runner_lease() {
    local lease_file
    [ -n "${VMID:-}" ] && [ -n "${RUNNER_NAME:-}" ] || return 0
    lease_file="${RUNNER_LEASE_DIR}/${VMID}.lease"
    if [ -r "$lease_file" ] \
        && grep -Fxq "pid=$$" "$lease_file" \
        && grep -Fxq "runner=${RUNNER_NAME}" "$lease_file"; then
        rm -f -- "$lease_file"
    fi
}
trap 'cleanup; remove_runner_lease' EXIT

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
if [ "$KEEP" = 1 ]; then
    install -d -o root -g root -m 0700 "$RUNNER_KEEP_DIR" \
        || die "cannot create durable keep directory"
    KEEP_TMP="$(mktemp "${RUNNER_KEEP_DIR}/.${VMID}.keep.XXXXXX")" \
        || die "cannot allocate durable keep marker"
    printf 'runner=%s\n' "$RUNNER_NAME" >"$KEEP_TMP" \
        && chmod 0600 "$KEEP_TMP" \
        && mv -f -- "$KEEP_TMP" "${RUNNER_KEEP_DIR}/${VMID}.keep" \
        || { rm -f -- "$KEEP_TMP"; die "cannot publish durable keep marker"; }
else
    rm -f -- "${RUNNER_KEEP_DIR}/${VMID}.keep" \
        || die "cannot clear stale durable keep marker for new clone"
fi
# For --keep, publish the generation-bound disposition before making this
# clone eligible for automatic recovery. A crash in the opposite order could
# leave an explicitly retained clone with recovery provenance but no marker.
# For the normal path, clearing the prior generation's marker first is also
# fail-closed: a crash before this description leaves a legacy/no-scope clone
# that the reaper only reports and never mutates.
qm set "$VMID" \
    --description "pulp-runner-generation=${RUNNER_NAME};pulp-runner-scope=${REGISTRATION_API}" \
    >/dev/null || die "cannot persist clone generation and registration scope"
install -d -o root -g root -m 0755 "$RUNNER_LEASE_DIR" \
    || die "cannot create runner lease directory"
LEASE_TMP="$(mktemp "${RUNNER_LEASE_DIR}/.${VMID}.lease.XXXXXX")" \
    || die "cannot allocate runner lease"
printf 'pid=%s\nrunner=%s\n' "$$" "$RUNNER_NAME" >"$LEASE_TMP" \
    || die "cannot write runner lease"
chmod 0600 "$LEASE_TMP" || die "cannot secure runner lease"
mv -f -- "$LEASE_TMP" "${RUNNER_LEASE_DIR}/${VMID}.lease" \
    || die "cannot publish runner lease"

# Keep the allocation lock until the clone is attached and its active firewall
# policy is proved. The host-network rollback takes this same lock before its
# empty-bridge proof, so it cannot tear down a bridge between clone allocation
# and attachment.
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
flock -u 9
[ "$AUTOMATIC_NETWORK_ISOLATION" != 1 ] \
    || flock -u 8 \
    || die "cannot release the host-network lock"

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

# GitHub's JIT endpoint creates one exact ephemeral registration. The
# generation UUID in RUNNER_NAME prevents a stale registration from causing a
# stable-name 409. Keep both request and response in mode-0600 files, and move
# the encoded one-use configuration into the guest over stdin only.
log "minting JIT runner configuration"
umask 077
JIT_REQUEST_FILE="$(mktemp)" || die "could not allocate JIT request file"
JIT_CONFIG_FILE="$(mktemp)" || die "could not allocate JIT config file"
python3 - "$JIT_REQUEST_FILE" "$RUNNER_NAME" "$RUNNER_GROUP_ID" "$LABELS" <<'PY'
import json
import pathlib
import sys

path, name, group_id, labels = sys.argv[1:]
payload = {
    "name": name,
    "runner_group_id": int(group_id or "1"),
    "labels": [label for label in labels.split(",") if label],
    "work_folder": "_work",
}
pathlib.Path(path).write_text(json.dumps(payload, separators=(",", ":")))
PY
github_api --method POST \
    "${REGISTRATION_API}/actions/runners/generate-jitconfig" \
    --input "$JIT_REQUEST_FILE" --jq .encoded_jit_config >"$JIT_CONFIG_FILE" \
    || die "could not mint JIT runner configuration"
[ -s "$JIT_CONFIG_FILE" ] || die "JIT response did not contain encoded configuration"
rm -f -- "$JIT_REQUEST_FILE"
JIT_REQUEST_FILE=""

log "starting ephemeral JIT runner ${RUNNER_NAME} (slot ${RUNNER_SLOT_ID}) on $VMID"
JIT_GUEST_FILE=/tmp/tartci-jit-config
ssh -o BatchMode=yes "ci@$GUEST_IP" \
    "umask 077; install -m 600 /dev/stdin ${JIT_GUEST_FILE}" \
    <"$JIT_CONFIG_FILE" \
    || die "could not transfer the short-lived JIT configuration"
rm -f -- "$JIT_CONFIG_FILE"
JIT_CONFIG_FILE=""
RUNNER_OUTPUT_FILE="$(mktemp)" || die "could not allocate runner output file"
ssh -o BatchMode=yes "ci@$GUEST_IP" "
    cd ~/actions-runner
    trap 'rm -f ${JIT_GUEST_FILE}' EXIT
    encoded_jit_config=\$(cat ${JIT_GUEST_FILE})
    ./run.sh --jitconfig \"\${encoded_jit_config}\"
" >"$RUNNER_OUTPUT_FILE" 2>&1 &
RUNNER_PID=$!

# A live ssh child is not proof that GitHub can dispatch to the registration.
# Bound the visibility wait and surface only a sanitized tail on failure.
RUNNER_READY=0
ready_deadline=$((SECONDS + RUNNER_READY_TIMEOUT_SECONDS))
while [ "$SECONDS" -lt "$ready_deadline" ]; do
    runners_tsv="$(github_api --paginate \
        "${REGISTRATION_API}/actions/runners?per_page=100" \
        --jq '.runners[] | [.id,.name,.status,.busy] | @tsv' 2>/dev/null)" \
        || runners_tsv=""
    runner_lookup="$(printf '%s\n' "$runners_tsv" | awk -F '\t' \
        -v name="$RUNNER_NAME" '$2 == name')"
    if [ -n "$runner_lookup" ]; then
        [ "$(printf '%s\n' "$runner_lookup" | wc -l | tr -d ' ')" = 1 ] \
            || die "duplicate generation-unique runner registrations"
        IFS=$'\t' read -r _ _ ready_status ready_busy <<< "$runner_lookup"
        if [ "$ready_status" = online ]; then
            RUNNER_READY=1
            log "JIT runner is visible to GitHub (online, busy=${ready_busy})"
            break
        fi
    fi
    kill -0 "$RUNNER_PID" 2>/dev/null || break
    sleep 2
done
if [ "$RUNNER_READY" != 1 ]; then
    kill "$RUNNER_PID" 2>/dev/null || true
    wait "$RUNNER_PID" 2>/dev/null || true
    tail -40 "$RUNNER_OUTPUT_FILE" 2>/dev/null | sanitize_runner_output >&2
    die "JIT runner never became visible to GitHub"
fi

# Two consecutive absent/offline observations fail closed. A busy observation
# resets the watchdog so cleanup never tears down a job that GitHub assigned.
HEARTBEAT_FAILURE_FILE="$(mktemp)" || die "could not allocate heartbeat state"
(
    monitor_runner_heartbeat
) &
HEARTBEAT_PID=$!

wait "$RUNNER_PID"
runner_exit=$?
kill "$HEARTBEAT_PID" 2>/dev/null || true
wait "$HEARTBEAT_PID" 2>/dev/null || true
HEARTBEAT_PID=""
heartbeat_failure="$(cat "$HEARTBEAT_FAILURE_FILE" 2>/dev/null || true)"
if [ -n "$heartbeat_failure" ]; then
    log "ERROR: JIT runner lost GitHub heartbeat ($heartbeat_failure)"
    die "JIT runner heartbeat failed before the job completed"
fi
if [ "$runner_exit" -ne 0 ]; then
    tail -80 "$RUNNER_OUTPUT_FILE" 2>/dev/null | sanitize_runner_output >&2
    die "JIT runner exited with status $runner_exit"
fi

# ── run exactly one job ──────────────────────────────────────────────────────
log "one JIT job finished on $VMID"
sanitize_runner_output <"$RUNNER_OUTPUT_FILE" | sed 's/^/    /'
