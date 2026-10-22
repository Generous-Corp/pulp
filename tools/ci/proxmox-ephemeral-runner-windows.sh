#!/usr/bin/env bash
# pulp-windows-ephemeral-runner — one Windows x64 CI job on a Proxmox VM that is
# reset to a golden snapshot before and after the job.
#
# The Linux pool clones a template per job. A Windows guest with the MSVC
# toolchain is a 130 GB disk set, so cloning it per job would exhaust the thin
# pool; the Windows slot instead owns one VM and rolls it back to a read-only
# snapshot. The guarantee is the same: nothing a job writes reaches the next job,
# and the golden cannot be poisoned by a job.
#
# Per invocation:
#   1. refuse while any registration from this slot is busy (a job is running)
#   2. stop the VM if a previous invocation left it running
#   3. ask macpro-governor.sh to admit the VM
#   4. roll back to the golden snapshot and boot it
#   5. mint a repository-scoped JIT config and run exactly one job over SSH
#   6. deregister an idle runner (the dispatch fence), stop the VM, roll back
#
# systemd restarts the unit after every job; that loop is the pool.
#
#   proxmox-ephemeral-runner-windows.sh          # run one job, then reset
#   proxmox-ephemeral-runner-windows.sh --keep   # leave the VM up after the job
#
# Configuration comes from /etc/pulp/windows-runner-%i.env (see the unit):
#   PULP_WINDOWS_VMID              VM to own (required)
#   PULP_WINDOWS_GOLDEN_SNAPSHOT   snapshot every job starts from (required)
#   PULP_WINDOWS_RUNNER_LABELS     default self-hosted,Windows,X64,pulp-build-windows-x64,pulp-host-macpro
#   PULP_WINDOWS_RUNNER_DIR        runner install dir in the guest, default B:\actions-runner
#   PULP_WINDOWS_GUEST_USER        SSH user in the guest (an administrator), default ci
#
set -uo pipefail

VMID="${PULP_WINDOWS_VMID:-}"
GOLDEN_SNAPSHOT="${PULP_WINDOWS_GOLDEN_SNAPSHOT:-}"
REPO="${PULP_RUNNER_REPO:-Generous-Corp/pulp}"
LABELS="${PULP_WINDOWS_RUNNER_LABELS:-self-hosted,Windows,X64,pulp-build-windows-x64,pulp-host-macpro}"
RUNNER_NAME_PREFIX="${PULP_WINDOWS_RUNNER_NAME_PREFIX:-pulp-win-ephemeral}"
RUNNER_DIR="${PULP_WINDOWS_RUNNER_DIR:-B:\\actions-runner}"
GUEST_USER="${PULP_WINDOWS_GUEST_USER:-ci}"
PAT_FILE="${PULP_RUNNER_PAT_FILE:-/root/.config/pulp/secrets/gh-runner-pat}"
GH_CLI="${PULP_WINDOWS_GH_CLI:-gh}"
QM="${PULP_WINDOWS_QM:-qm}"
SSH_BIN="${PULP_WINDOWS_SSH:-ssh}"
FLOCK="${PULP_WINDOWS_FLOCK:-flock}"
GOVERNOR="${PULP_WINDOWS_GOVERNOR:-/usr/local/sbin/macpro-governor.sh}"
STATE_DIR="${PULP_WINDOWS_STATE_DIR:-/var/lib/pulp/windows-runner}"
LOCK_DIR="${PULP_WINDOWS_LOCK_DIR:-/var/lock}"
RUNNER_READY_TIMEOUT_SECONDS="${PULP_WINDOWS_RUNNER_READY_TIMEOUT_SECONDS:-180}"
RUNNER_HEARTBEAT_INTERVAL_SECONDS="${PULP_WINDOWS_RUNNER_HEARTBEAT_INTERVAL_SECONDS:-15}"
GUEST_BOOT_TIMEOUT_SECONDS="${PULP_WINDOWS_GUEST_BOOT_TIMEOUT_SECONDS:-600}"
BUSY_WAIT_SECONDS="${PULP_WINDOWS_BUSY_WAIT_SECONDS:-4800}"
GOVERNOR_REFUSAL_BACKOFF_SECONDS="${PULP_GOVERNOR_REFUSAL_BACKOFF_SECONDS:-270}"
POLL_SECONDS="${PULP_WINDOWS_POLL_SECONDS:-5}"
KEEP=0
PAT=""
VM_STARTED=0
GUEST_IP=""

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*"; }
die() { log "ERROR: $*"; exit 1; }

sanitize_runner_output() {
    sed -E \
        -e 's/(token|authorization|credentials|jitconfig|encoded_jit_config)[=:][^[:space:]]+/\1=<redacted>/Ig' \
        -e 's/(A[A-Za-z0-9_-]{20,})/<redacted>/g'
}

github_api() {
    GH_TOKEN="$PAT" "$GH_CLI" api "$@"
}

# One host-key file per VM, under a fixed alias: the golden's key is stable
# across rollbacks, so after the first boot a different key means a different
# machine answered on the DHCP address, and SSH refuses it.
guest_ssh() {
    "$SSH_BIN" -o BatchMode=yes -o ConnectTimeout=10 \
        -o ServerAliveInterval=30 -o ServerAliveCountMax=6 \
        -o StrictHostKeyChecking=accept-new \
        -o UserKnownHostsFile="${STATE_DIR}/known_hosts" \
        -o HostKeyAlias="pulp-windows-vm-${VMID}" \
        "${GUEST_USER}@${GUEST_IP}" "$@"
}

# runners_tsv: id, name, status, busy for every repository runner.
runners_tsv() {
    github_api --paginate "repos/${REPO}/actions/runners?per_page=100" \
        --jq '.runners[] | [.id,.name,.status,.busy] | @tsv'
}

# Registrations whose name starts with this slot's prefix.
slot_registrations() {
    local tsv
    tsv="$(runners_tsv)" || return 1
    printf '%s\n' "$tsv" | awk -F '\t' -v prefix="${RUNNER_SLOT_ID}-" 'index($2, prefix) == 1'
}

# Dispatch fence for an idle JIT runner, as in the Linux supervisor: a DELETE
# that succeeds proves the runner was idle at that instant, and no job can be
# assigned to a runner that no longer exists. Returns 0 when fenced, 3 when the
# runner is busy, 1 when a step cannot be proved.
jit_deregister_fence() {
    local rid="$1" runner_name="$2" record name busy output run_ids run_id job_runners
    record="$(github_api "repos/${REPO}/actions/runners/${rid}" \
        --jq '[.id,.name,.busy] | @tsv' 2>/dev/null)" \
        || { log "fence: cannot read runner id $rid"; return 1; }
    IFS=$'\t' read -r _ name busy <<< "$record"
    [ "$name" = "$runner_name" ] || { log "fence: runner id $rid is $name, not $runner_name"; return 1; }
    [ "$busy" = false ] || { log "fence: runner id $rid is busy"; return 3; }
    if ! output="$(github_api --method DELETE "repos/${REPO}/actions/runners/${rid}" 2>&1)"; then
        case "$output" in
            *[Bb]usy*|*"running a job"*|*"HTTP 422"*) log "fence: GitHub refused to deregister busy runner $rid"; return 3 ;;
        esac
        log "fence: cannot deregister runner id $rid"
        return 1
    fi
    if output="$(github_api "repos/${REPO}/actions/runners/${rid}" 2>&1)"; then
        log "fence: runner id $rid still exists after deregistration"
        return 1
    fi
    case "$output" in
        *"HTTP 404"*|*"Not Found"*) ;;
        *) log "fence: runner id $rid lookup failed for a reason other than 404"; return 1 ;;
    esac
    run_ids="$(github_api --paginate "repos/${REPO}/actions/runs?status=in_progress&per_page=100" \
        --jq '.workflow_runs[].id' 2>/dev/null)" \
        || { log "fence: cannot list in-progress runs"; return 1; }
    for run_id in $run_ids; do
        [[ "$run_id" =~ ^[0-9]+$ ]] || { log "fence: invalid run id"; return 1; }
        job_runners="$(github_api --paginate "repos/${REPO}/actions/runs/${run_id}/jobs?per_page=100" \
            --jq '.jobs[] | select(.status == "in_progress") | .runner_name // empty' 2>/dev/null)" \
            || { log "fence: cannot list jobs of run $run_id"; return 1; }
        if printf '%s\n' "$job_runners" | grep -Fxq -- "$runner_name"; then
            log "fence: an in-progress job in run $run_id names $runner_name"
            return 1
        fi
    done
    log "fence: deregistered idle runner $runner_name (id $rid)"
}

# Fence every registration of this slot. A busy one is waited on, never killed:
# GitHub assigned it a job, and stopping the VM under it would lose that job.
fence_slot_registrations() {
    local deadline=$((SECONDS + BUSY_WAIT_SECONDS)) matches rid name status busy rc
    while :; do
        matches="$(slot_registrations)" || { log "cannot list registrations for ${RUNNER_SLOT_ID}"; return 1; }
        [ -n "$matches" ] || return 0
        rc=0
        while IFS=$'\t' read -r rid name status busy; do
            [ -n "$rid" ] || continue
            if [ "$busy" = true ]; then
                rc=3
                continue
            fi
            jit_deregister_fence "$rid" "$name"
            case $? in
                0) ;;
                3) rc=3 ;;
                *) return 1 ;;
            esac
        done <<< "$matches"
        [ "$rc" = 3 ] || return 0
        [ "$SECONDS" -lt "$deadline" ] || { log "slot ${RUNNER_SLOT_ID} stayed busy for ${BUSY_WAIT_SECONDS}s"; return 3; }
        log "a registration of ${RUNNER_SLOT_ID} is running a job; waiting"
        sleep 30
    done
}

vm_status() {
    "$QM" status "$VMID" 2>/dev/null | awk '{ print $2 }'
}

stop_vm() {
    local status
    status="$(vm_status)"
    [ "$status" = stopped ] && return 0
    "$QM" shutdown "$VMID" --timeout 120 >/dev/null 2>&1 \
        || "$QM" stop "$VMID" >/dev/null 2>&1 || true
    for _ in $(seq 1 60); do
        [ "$(vm_status)" = stopped ] && return 0
        sleep 2
    done
    return 1
}

# Discard everything the job wrote. Rollback needs the VM stopped.
reset_vm() {
    stop_vm || { log "ERROR: VM $VMID did not stop; not rolling back"; return 1; }
    "$QM" rollback "$VMID" "$GOLDEN_SNAPSHOT" >/dev/null \
        || { log "ERROR: rollback of VM $VMID to $GOLDEN_SNAPSHOT failed"; return 1; }
    log "VM $VMID reset to $GOLDEN_SNAPSHOT"
}

# The guest's first non-link-local IPv4 address, from the QEMU guest agent.
guest_ipv4() {
    "$QM" guest cmd "$VMID" network-get-interfaces 2>/dev/null | python3 -c '
import json, sys
try:
    interfaces = json.load(sys.stdin)
except ValueError:
    sys.exit(1)
for interface in interfaces:
    if interface.get("name", "").lower().startswith("loopback"):
        continue
    for address in interface.get("ip-addresses", []):
        ip = address.get("ip-address", "")
        if address.get("ip-address-type") == "ipv4" and not ip.startswith(("127.", "169.254.")):
            print(ip)
            sys.exit(0)
sys.exit(1)
'
}

cleanup() {
    local rc=$?
    [ -z "${HEARTBEAT_PID:-}" ] || kill "$HEARTBEAT_PID" 2>/dev/null || true
    [ -z "${RUNNER_PID:-}" ] || kill "$RUNNER_PID" 2>/dev/null || true
    rm -f -- "${JIT_REQUEST_FILE:-}" "${JIT_CONFIG_FILE:-}" \
        "${RUNNER_OUTPUT_FILE:-}" "${HEARTBEAT_FAILURE_FILE:-}"
    [ "$VM_STARTED" = 1 ] || exit "$rc"
    if [ "$KEEP" = 1 ]; then
        log "--keep set: leaving VM $VMID up at ${GUEST_IP:-unknown address}"
        exit "$rc"
    fi
    fence_slot_registrations
    case $? in
        0) reset_vm || rc=1 ;;
        *) log "ERROR: cannot prove ${RUNNER_SLOT_ID} is idle; leaving VM $VMID running for the next invocation"; rc=1 ;;
    esac
    exit "$rc"
}

monitor_runner_heartbeat() {
    local misses=0 lookup status busy
    while kill -0 "$RUNNER_PID" 2>/dev/null; do
        if lookup="$(runners_tsv 2>/dev/null)"; then
            lookup="$(printf '%s\n' "$lookup" | awk -F '\t' -v name="$RUNNER_NAME" '$2 == name')"
            status="" busy=""
            [ -z "$lookup" ] || IFS=$'\t' read -r _ _ status busy <<< "$lookup"
            if [ "$busy" = true ] || [ "$status" = online ]; then
                misses=0
            else
                misses=$((misses + 1))
                if [ "$misses" -ge 2 ]; then
                    printf 'runner=%s status=%s busy=%s\n' "$RUNNER_NAME" \
                        "${status:-missing}" "${busy:-unknown}" >"$HEARTBEAT_FAILURE_FILE"
                    kill "$RUNNER_PID" 2>/dev/null || true
                    break
                fi
            fi
        fi
        sleep "$RUNNER_HEARTBEAT_INTERVAL_SECONDS"
    done
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --keep) KEEP=1; shift ;;
        --once) shift ;;
        --help|-h) sed -n '2,31p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[[ "$VMID" =~ ^[1-9][0-9]*$ ]] || die "PULP_WINDOWS_VMID must be a VM id"
[[ "$GOLDEN_SNAPSHOT" =~ ^[A-Za-z][A-Za-z0-9_-]*$ ]] \
    || die "PULP_WINDOWS_GOLDEN_SNAPSHOT must name a snapshot"
case "$REPO" in
    */*/*|/*|.*|*' '*|'') die "runner repository must be OWNER/REPO: $REPO" ;;
    */*) ;;
    *) die "runner repository must be OWNER/REPO: $REPO" ;;
esac
[[ "$RUNNER_NAME_PREFIX" =~ ^[A-Za-z0-9._-]+$ ]] || die "runner name prefix must be shell-safe"
[[ "$GUEST_USER" =~ ^[A-Za-z0-9._-]+$ ]] || die "guest user must be shell-safe"
[[ "$RUNNER_DIR" =~ ^[A-Za-z]:\\[A-Za-z0-9._\\-]+$ ]] || die "runner directory must be an absolute Windows path"
for required_label in self-hosted Windows X64; do
    case ",$LABELS," in
        *",${required_label},"*) ;;
        *) die "Windows runner labels must include self-hosted,Windows,X64" ;;
    esac
done
# This slot registers at repository scope. The capability labels that route
# automatic PR work are reserved for runners in a verified organization group.
case ",$LABELS," in
    *,pulp-auto-*|*,pulp-pr-safe-*) die "automatic-routing labels require a verified organization runner group" ;;
esac
[ -f "$PAT_FILE" ] && [ ! -L "$PAT_FILE" ] && [ -r "$PAT_FILE" ] \
    || die "runner credential $PAT_FILE must be a readable regular file"
if [ -z "${PULP_WINDOWS_SKIP_CREDENTIAL_MODE_CHECK:-}" ]; then
    [ "$(stat -c '%u:%a' -- "$PAT_FILE" 2>/dev/null)" = "0:600" ] \
        || die "runner credential must be root-owned mode 0600"
fi
PAT="$(cat "$PAT_FILE")"
[ -n "$PAT" ] || die "runner credential is empty"
command -v "$GH_CLI" >/dev/null 2>&1 || die "$GH_CLI is not on PATH"
mkdir -p "$STATE_DIR" || die "cannot create $STATE_DIR"

RUNNER_SLOT_ID="${RUNNER_NAME_PREFIX}-${VMID}"

# One supervisor per VM. A second copy waits here instead of racing the first
# one's rollback.
exec 9>"${LOCK_DIR}/pulp-windows-runner-${VMID}.lock" || die "cannot open the VM lock"
"$FLOCK" -w 60 9 || die "another supervisor owns VM $VMID"

"$QM" listsnapshot "$VMID" 2>/dev/null | grep -Eq -- "(^|[[:space:]])${GOLDEN_SNAPSHOT}([[:space:]]|$)" \
    || die "VM $VMID has no snapshot $GOLDEN_SNAPSHOT"

# A previous invocation that died mid-job can leave the VM running with a
# registration that is still busy. Wait for it rather than reset under it.
fence_slot_registrations
case $? in
    0) ;;
    3) log "slot is still running a job; retrying later"; exit 75 ;;
    *) die "cannot prove ${RUNNER_SLOT_ID} has no live registration" ;;
esac
reset_vm || die "cannot reset VM $VMID before the job"

# ── admission ────────────────────────────────────────────────────────────────
# A refusal waits before exiting so a host that stays full retries about every
# five minutes instead of every RestartSec.
if ! admission="$("$GOVERNOR" can-start "$VMID" 2>&1)"; then
    log "REFUSED by governor for VM $VMID"
    printf '%s\n' "$admission" | sed 's/^/    /'
    log "retrying admission in ${GOVERNOR_REFUSAL_BACKOFF_SECONDS}s"
    sleep "$GOVERNOR_REFUSAL_BACKOFF_SECONDS"
    exit 75
fi

RUNNER_NAME="${RUNNER_SLOT_ID}-$(python3 -c 'import uuid; print(uuid.uuid4())')"
[ "${#RUNNER_NAME}" -le 64 ] || die "runner name exceeds GitHub's 64-character limit"

trap cleanup EXIT
trap 'exit 143' TERM INT
"$QM" set "$VMID" --description "pulp-runner-generation=${RUNNER_NAME};pulp-runner-snapshot=${GOLDEN_SNAPSHOT}" \
    >/dev/null 2>&1 || log "WARN: could not record the runner generation on VM $VMID"
"$QM" start "$VMID" >/dev/null || die "start of VM $VMID failed"
VM_STARTED=1

# ── wait for the guest ───────────────────────────────────────────────────────
boot_deadline=$((SECONDS + GUEST_BOOT_TIMEOUT_SECONDS))
while [ "$SECONDS" -lt "$boot_deadline" ]; do
    GUEST_IP="$(guest_ipv4)" && [ -n "$GUEST_IP" ] && guest_ssh 'exit 0' >/dev/null 2>&1 && break
    GUEST_IP=""
    sleep "$POLL_SECONDS"
done
[ -n "$GUEST_IP" ] || die "VM $VMID did not answer SSH within ${GUEST_BOOT_TIMEOUT_SECONDS}s"
log "VM $VMID up at $GUEST_IP"

# ── register ephemeral ───────────────────────────────────────────────────────
log "minting JIT runner configuration"
umask 077
JIT_REQUEST_FILE="$(mktemp)" || die "could not allocate JIT request file"
JIT_CONFIG_FILE="$(mktemp)" || die "could not allocate JIT config file"
python3 - "$JIT_REQUEST_FILE" "$RUNNER_NAME" "$LABELS" <<'PY'
import json
import pathlib
import sys

path, name, labels = sys.argv[1:]
payload = {
    "name": name,
    "runner_group_id": 1,
    "labels": [label for label in labels.split(",") if label],
    "work_folder": "_work",
}
pathlib.Path(path).write_text(json.dumps(payload, separators=(",", ":")))
PY
github_api --method POST "repos/${REPO}/actions/runners/generate-jitconfig" \
    --input "$JIT_REQUEST_FILE" --jq .encoded_jit_config >"$JIT_CONFIG_FILE" \
    || die "could not mint JIT runner configuration"
[ -s "$JIT_CONFIG_FILE" ] || die "JIT response did not contain encoded configuration"

# The one-use configuration travels over stdin only, never on a command line.
# The guest's SSH shell is PowerShell.
JIT_GUEST_FILE="${RUNNER_DIR}\\.jitconfig"
guest_ssh "[IO.File]::WriteAllText('${JIT_GUEST_FILE}', [Console]::In.ReadToEnd().Trim())" \
    <"$JIT_CONFIG_FILE" || die "could not transfer the JIT configuration"
rm -f -- "$JIT_CONFIG_FILE" "$JIT_REQUEST_FILE"
JIT_CONFIG_FILE="" JIT_REQUEST_FILE=""

log "starting ephemeral JIT runner ${RUNNER_NAME} on VM $VMID"
RUNNER_OUTPUT_FILE="$(mktemp)" || die "could not allocate runner output file"
guest_ssh "Set-Location '${RUNNER_DIR}'; \$j = Get-Content -Raw '.jitconfig'; Remove-Item '.jitconfig'; & .\\run.cmd --jitconfig \$j.Trim(); exit \$LASTEXITCODE" \
    </dev/null >"$RUNNER_OUTPUT_FILE" 2>&1 &
RUNNER_PID=$!

RUNNER_READY=0
ready_deadline=$((SECONDS + RUNNER_READY_TIMEOUT_SECONDS))
while [ "$SECONDS" -lt "$ready_deadline" ]; do
    lookup="$(runners_tsv 2>/dev/null | awk -F '\t' -v name="$RUNNER_NAME" '$2 == name')"
    if [ -n "$lookup" ]; then
        IFS=$'\t' read -r _ _ ready_status ready_busy <<< "$lookup"
        if [ "$ready_status" = online ]; then
            RUNNER_READY=1
            log "JIT runner is visible to GitHub (online, busy=${ready_busy})"
            break
        fi
    fi
    kill -0 "$RUNNER_PID" 2>/dev/null || break
    sleep "$POLL_SECONDS"
done
if [ "$RUNNER_READY" != 1 ]; then
    tail -40 "$RUNNER_OUTPUT_FILE" 2>/dev/null | sanitize_runner_output >&2
    die "JIT runner never became visible to GitHub"
fi

# Two consecutive absent/offline observations fail closed; a busy or online
# observation resets the count, so a job GitHub assigned is never torn down.
HEARTBEAT_FAILURE_FILE="$(mktemp)" || die "could not allocate heartbeat state"
monitor_runner_heartbeat &
HEARTBEAT_PID=$!

wait "$RUNNER_PID"
runner_exit=$?
RUNNER_PID=""
kill "$HEARTBEAT_PID" 2>/dev/null || true
wait "$HEARTBEAT_PID" 2>/dev/null || true
HEARTBEAT_PID=""
heartbeat_failure="$(cat "$HEARTBEAT_FAILURE_FILE" 2>/dev/null || true)"
[ -z "$heartbeat_failure" ] || die "JIT runner lost its GitHub heartbeat ($heartbeat_failure)"
if [ "$runner_exit" -ne 0 ]; then
    tail -80 "$RUNNER_OUTPUT_FILE" 2>/dev/null | sanitize_runner_output >&2
    die "JIT runner exited with status $runner_exit"
fi
log "one JIT job finished on VM $VMID"
sanitize_runner_output <"$RUNNER_OUTPUT_FILE" | tail -20 | sed 's/^/    /'
