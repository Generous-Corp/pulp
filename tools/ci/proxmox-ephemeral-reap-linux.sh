#!/usr/bin/env bash
# Recover orphaned one-job JIT clones without ever guessing that a runner is idle.
#
# The normal supervisor owns teardown. This reaper is only for a supervisor that
# disappeared and left its clone behind. Report-only is the safe interactive
# default; the systemd service uses --yes after every proof below succeeds.
set -uo pipefail

BASE="${PULP_REAPER_CLONE_BASE:-200}"
MAX="${PULP_REAPER_CLONE_MAX:-219}"
REPO="${PULP_REAPER_REPO:-Generous-Corp/pulp}"
ORG="${REPO%%/*}"
GH_CLI="${PULP_REAPER_GH_CLI:-/usr/local/bin/ghapp}"
QM="${PULP_REAPER_QM:-qm}"
SSH="${PULP_REAPER_SSH:-ssh}"
LEASE_DIR="${PULP_REAPER_LEASE_DIR:-/run/pulp-ephemeral-runner}"
KEEP_DIR="${PULP_REAPER_KEEP_DIR:-/var/lib/pulp/ephemeral-runner-keep}"
VM_CONFIG_DIR="${PULP_REAPER_VM_CONFIG_DIR:-/etc/pve/qemu-server}"
VMID_LOCK="${PULP_REAPER_VMID_LOCK:-/var/lock/pulp-ephemeral-vmid.lock}"
FIREWALL_DIR="${PULP_REAPER_FIREWALL_DIR:-/etc/pve/firewall}"
MIN_STALE_SECONDS="${PULP_REAPER_MIN_STALE_SECONDS:-3600}"
DO_IT=0
[ "${1:-}" = "--yes" ] && DO_IT=1

log() { printf '%s\n' "$*"; }
valid_uint() { [[ "$1" =~ ^[0-9]+$ ]]; }

github_helper_secure() {
    local metadata owner mode mode_value
    if [ "${PULP_REAPER_TEST_MODE:-0}" = 1 ]; then
        [ -f "$GH_CLI" ] && [ ! -L "$GH_CLI" ] && [ -x "$GH_CLI" ]
        return
    fi
    [ "$GH_CLI" = /usr/local/bin/ghapp ] \
        && [ -f "$GH_CLI" ] && [ ! -L "$GH_CLI" ] && [ -x "$GH_CLI" ] \
        || return 1
    metadata="$(stat -c '%u:%a' -- "$GH_CLI" 2>/dev/null)" || return 1
    owner="${metadata%%:*}"
    mode="${metadata#*:}"
    [ "$owner" = 0 ] && [[ "$mode" =~ ^[0-7]{3,4}$ ]] || return 1
    mode_value=$((8#$mode))
    (( (mode_value & 8#022) == 0 ))
}

scope_runners() {
    local registration_api="$1"
    env -u GH_TOKEN -u GITHUB_TOKEN -u GH_ENTERPRISE_TOKEN \
        -u GITHUB_ENTERPRISE_TOKEN HOME=/root \
        "$GH_CLI" api --paginate \
        "${registration_api}/actions/runners?per_page=100" \
        --jq '.runners[] | [.id,.name,.status,.busy] | @tsv'
}

runner_record() {
    local runner_name="$1" expected_scope="${2:-}" registration_api runners_tsv matches all_matches=""
    local -a scopes
    case "$expected_scope" in
        "orgs/${ORG}"|"repos/${REPO}") scopes=("$expected_scope") ;;
        "") scopes=("orgs/${ORG}" "repos/${REPO}") ;;
        *) return 2 ;;
    esac
    for registration_api in "${scopes[@]}"; do
        runners_tsv="$(scope_runners "$registration_api" 2>/dev/null)" || return 2
        matches="$(printf '%s\n' "$runners_tsv" | awk -F '\t' -v n="$runner_name" '$2 == n')"
        [ -n "$matches" ] || continue
        while IFS= read -r match; do
            all_matches+="${registration_api}"$'\t'"${match}"$'\n'
        done <<< "$matches"
    done
    [ -n "$all_matches" ] || return 1
    [ "$(printf '%s' "$all_matches" | wc -l | tr -d ' ')" = 1 ] || return 2
    printf '%s' "$all_matches"
}

lease_is_live() {
    local id="$1" lease pid runner proc_args metadata
    lease="${LEASE_DIR}/${id}.lease"
    [ -r "$lease" ] || return 1
    [ -f "$lease" ] && [ ! -L "$lease" ] || return 2
    metadata="$(stat -c '%u:%a' -- "$lease" 2>/dev/null)" || return 2
    [ "$metadata" = 0:600 ] || return 2
    pid="$(sed -n 's/^pid=//p' "$lease")"
    runner="$(sed -n 's/^runner=//p' "$lease")"
    valid_uint "$pid" && [[ "$runner" =~ ^[A-Za-z0-9._-]+$ ]] || return 2
    [ -r "/proc/${pid}/cmdline" ] || return 1
    proc_args="$(tr '\000' ' ' <"/proc/${pid}/cmdline")" || return 2
    case "$proc_args" in
        *pulp-ephemeral-runner*) return 0 ;;
        *) return 2 ;;
    esac
}

durable_keep_matches() {
    local id="$1" generation="$2" keep metadata kept_runner
    keep="${KEEP_DIR}/${id}.keep"
    [ -e "$keep" ] || return 1
    [ -f "$keep" ] && [ ! -L "$keep" ] && [ -r "$keep" ] || return 2
    metadata="$(stat -c '%u:%a' -- "$keep" 2>/dev/null)" || return 2
    [ "$metadata" = 0:600 ] || return 2
    kept_runner="$(sed -n 's/^runner=//p' "$keep")"
    [[ "$kept_runner" =~ ^[A-Za-z0-9._-]+$ ]] || return 2
    [ -n "$generation" ] && [ "$kept_runner" = "$generation" ] || return 2
    return 0
}

guest_probe() {
    local ip="$1"
    "$SSH" -o BatchMode=yes -o StrictHostKeyChecking=yes \
        -o UserKnownHostsFile=/root/.ssh/known_hosts -o ConnectTimeout=8 \
        "ci@${ip}" python3 - <<'PY'
import json
import pathlib
import re

import os

root = pathlib.Path(os.environ.get("TARTCI_PROBE_RUNNER_ROOT", "/home/ci/actions-runner"))
identity = ""
if (root / ".runner").exists():
    try:
        runner_config = json.loads((root / ".runner").read_text(encoding="utf-8-sig"))
        identity = runner_config.get("AgentName") or runner_config.get("agentName")
    except Exception:
        raise SystemExit(2)
    if not isinstance(identity, str) or not re.fullmatch(r"[A-Za-z0-9._-]+", identity):
        raise SystemExit(2)
else:
    # A JIT runner deletes .runner when its one job ends, so a finished clone has
    # none. Its listener log still names the runner it ran as. Take that name
    # only when every listener log agrees on exactly one, so the caller can
    # match it against the host's recorded generation.
    names = set()
    for log in sorted((root / "_diag").glob("Runner_*.log")):
        try:
            text = log.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        names.update(re.findall(r'"AgentName":\s*"([A-Za-z0-9._-]+)"', text))
    if len(names) == 1:
        identity = names.pop()
    elif len(names) > 1:
        raise SystemExit(2)

listeners = []
workers = 0
configurers = 0
proc_root = pathlib.Path("/proc")
for proc in (proc_root.iterdir() if proc_root.is_dir() else ()):
    if not proc.name.isdigit():
        continue
    try:
        comm = (proc / "comm").read_text().strip()
        argv = (proc / "cmdline").read_bytes().split(b"\0")
    except (FileNotFoundError, PermissionError, ProcessLookupError):
        continue
    if comm == "Runner.Listener":
        listeners.append(argv)
    elif comm == "Runner.Worker":
        workers += 1
    if any(arg.endswith(b"/config.sh") or arg == b"./config.sh" for arg in argv):
        configurers += 1

work = root / "_work"
work_entries = sum(1 for _ in work.rglob("*")) if work.exists() else 0
jit = len(listeners) == 1 and b"--jitconfig" in listeners[0]
print(f"identity={identity}")
print(f"listener_count={len(listeners)}")
print(f"worker_count={workers}")
print(f"configurer_count={configurers}")
print(f"jitconfig={'true' if jit else 'false'}")
print(f"work_entries={work_entries}")
PY
}

probe_value() {
    local probe="$1" key="$2"
    printf '%s\n' "$probe" | sed -n "s/^${key}=//p"
}

gh_api() {
    env -u GH_TOKEN -u GITHUB_TOKEN -u GH_ENTERPRISE_TOKEN \
        -u GITHUB_ENTERPRISE_TOKEN HOME=/root "$GH_CLI" api "$@"
}

# Dispatch fence for an idle JIT runner: deregistration itself.
#
# A JIT runner's labels come from its JIT configuration and are read-only, so a
# fence that replaces labels can never remove the routing labels and can never
# prove anything; it left every idle orphan unreclaimable. GitHub refuses to
# delete a runner while it is running a job, so a DELETE that succeeds proves no
# job held the runner at that instant, and no job can be assigned to a runner
# that no longer exists. The 404 alone could also mean the wrong id was read, so
# the fence also requires that no in-progress job in the repository names the
# runner. Every step is logged; any step that cannot be proved leaves the clone.
#
# deregister_fence <vmid> <registration_api> <rid> <runner_name>
deregister_fence() {
    local id="$1" registration_api="$2" rid="$3" runner_name="$4"
    local record name busy output run_ids run_id job_runners
    record="$(gh_api "${registration_api}/actions/runners/${rid}" \
        --jq '[.id,.name,.busy] | @tsv' 2>/dev/null)" \
        || { log "SKIP $id — JIT fence 1/4: runner ${rid} cannot be read"; return 1; }
    IFS=$'\t' read -r _ name busy <<< "$record"
    [ "$name" = "$runner_name" ] \
        || { log "SKIP $id — JIT fence 1/4: runner ${rid} is ${name}, not ${runner_name}"; return 1; }
    [ "$busy" = false ] \
        || { log "SKIP $id — JIT fence 1/4: runner ${rid} is busy; leaving it this pass"; return 1; }
    log "FENCE $id 1/4 read runner ${rid} ${runner_name} busy=false"

    if ! output="$(gh_api --method DELETE "${registration_api}/actions/runners/${rid}" 2>&1)"; then
        case "$output" in
            *[Bb]usy*|*"running a job"*|*"HTTP 422"*)
                log "SKIP $id — JIT fence 2/4: GitHub refused to deregister runner ${rid} because it is busy; leaving it this pass" ;;
            *)
                log "SKIP $id — JIT fence 2/4: deregistering runner ${rid} failed" ;;
        esac
        return 1
    fi
    log "FENCE $id 2/4 deregistered runner ${rid}"

    if output="$(gh_api "${registration_api}/actions/runners/${rid}" 2>&1)"; then
        log "SKIP $id — JIT fence 3/4: runner ${rid} still exists after deregistration"
        return 1
    fi
    case "$output" in
        *"HTTP 404"*|*"Not Found"*) ;;
        *) log "SKIP $id — JIT fence 3/4: runner ${rid} lookup failed for a reason other than 404"; return 1 ;;
    esac
    run_ids="$(gh_api --paginate "repos/${REPO}/actions/runs?status=in_progress&per_page=100" \
        --jq '.workflow_runs[].id' 2>/dev/null)" \
        || { log "SKIP $id — JIT fence 3/4: in-progress runs cannot be listed"; return 1; }
    for run_id in $run_ids; do
        valid_uint "$run_id" \
            || { log "SKIP $id — JIT fence 3/4: invalid run id"; return 1; }
        job_runners="$(gh_api --paginate "repos/${REPO}/actions/runs/${run_id}/jobs?per_page=100" \
            --jq '.jobs[] | select(.status == "in_progress") | .runner_name // empty' 2>/dev/null)" \
            || { log "SKIP $id — JIT fence 3/4: jobs of run ${run_id} cannot be listed"; return 1; }
        if printf '%s\n' "$job_runners" | grep -Fxq -- "$runner_name"; then
            log "SKIP $id — JIT fence 3/4: in-progress job in run ${run_id} names ${runner_name}"
            return 1
        fi
    done
    log "FENCE $id 3/4 runner ${rid} returns 404 and no in-progress job names ${runner_name}"
}

valid_uint "$BASE" && valid_uint "$MAX" && valid_uint "$MIN_STALE_SECONDS" \
    && [ "$BASE" -le "$MAX" ] || { log "ERROR invalid reaper bounds"; exit 2; }
github_helper_secure \
    || { log "ERROR trusted GitHub App helper is unavailable or insecure"; exit 2; }

for id in $(seq "$BASE" "$MAX"); do
    "$QM" status "$id" >/dev/null 2>&1 || continue
    vm_config="${VM_CONFIG_DIR}/${id}.conf"
    [ -f "$vm_config" ] || { log "SKIP $id — VM config is unavailable"; continue; }
    vm_name="$($QM config "$id" 2>/dev/null | sed -n 's/^name: //p')"
    [ "$vm_name" = "pulp-ci-ephemeral-${id}" ] \
        || { log "SKIP $id — VM identity is not the Pulp ephemeral slot"; continue; }
    # Snapshot the host-owned generation before inspecting any guest or GitHub
    # state. A predecessor can disappear and its VMID be reused while those
    # network proofs run; only this original digest may authorize teardown.
    config_digest="$(sha256sum "$vm_config" | awk '{print $1}')" \
        || { log "SKIP $id — VM generation cannot be hashed"; continue; }
    config_mtime="$(stat -c %Y "$vm_config" 2>/dev/null \
        || stat -f %m "$vm_config" 2>/dev/null)" \
        || { log "SKIP $id — VM config timestamp is unavailable"; continue; }
    valid_uint "$config_mtime" \
        || { log "SKIP $id — VM config timestamp is invalid"; continue; }
    age=$(( $(date +%s) - config_mtime ))
    [ "$age" -ge "$MIN_STALE_SECONDS" ] \
        || { log "SKIP $id — clone is only ${age}s old"; continue; }

    lease_is_live "$id"
    lease_status=$?
    [ "$lease_status" -eq 1 ] \
        || { log "SKIP $id — supervisor lease is active or ambiguous"; continue; }

    host_description="$($QM config "$id" 2>/dev/null | sed -n 's/^description: //p')"
    host_generation="$(printf '%s\n' "$host_description" \
        | sed -n 's/^pulp-runner-generation=\([^;]*\).*/\1/p')"
    host_scope="$(printf '%s\n' "$host_description" \
        | sed -n 's/.*;pulp-runner-scope=\([^;]*\).*/\1/p')"
    durable_keep_matches "$id" "$host_generation"
    keep_status=$?
    [ "$keep_status" -eq 1 ] \
        || { log "SKIP $id — durable keep disposition is active or ambiguous"; continue; }
    vm_status="$($QM status "$id" 2>/dev/null)" \
        || { log "SKIP $id — VM status is unavailable"; continue; }
    cleanup_state=""
    registration_present=0
    ip=""
    runner_name="$host_generation"

    if [ "$vm_status" = "status: stopped" ]; then
        [[ "$host_generation" =~ ^[A-Za-z0-9._-]+$ ]] \
            || { log "SKIP $id — stopped legacy clone lacks a host generation"; continue; }
        record="$(runner_record "$runner_name" "$host_scope")"; record_status=$?
        if [ "$record_status" -eq 1 ]; then
            cleanup_state="stopped-post-job"
        elif [ "$record_status" -eq 0 ]; then
            IFS=$'\t' read -r registration_api rid _ status busy <<< "$record"
            [ "$busy" = false ] && [ "$status" = offline ] \
                || { log "SKIP $id — stopped clone registration is not idle and offline"; continue; }
            registration_present=1
            cleanup_state="stopped-idle-registration"
        else
            log "SKIP $id — stopped clone registration is ambiguous"
            continue
        fi
    elif [ "$vm_status" = "status: running" ]; then
        ip="$($QM guest cmd "$id" network-get-interfaces 2>/dev/null \
            | grep -oE '(10\.240\.[0-9]+\.[0-9]+|192\.168\.[0-9]+\.[0-9]+)' | head -1)"
        [ -n "$ip" ] || { log "SKIP $id — guest address is unavailable"; continue; }
        probe="$(guest_probe "$ip" 2>/dev/null)" \
            || { log "SKIP $id — guest state cannot be proved"; continue; }
        guest_identity="$(probe_value "$probe" identity)"
        listener_count="$(probe_value "$probe" listener_count)"
        worker_count="$(probe_value "$probe" worker_count)"
        configurer_count="$(probe_value "$probe" configurer_count)"
        jitconfig="$(probe_value "$probe" jitconfig)"
        work_entries="$(probe_value "$probe" work_entries)"
        [ "$worker_count" = 0 ] && [ "$configurer_count" = 0 ] \
            || { log "SKIP $id — guest has an active worker or configurer"; continue; }

        if [ "$listener_count" = 1 ] && [ "$jitconfig" = true ] \
            && [ "$work_entries" = 0 ]; then
            runner_name="$guest_identity"
            if [ -n "$host_generation" ]; then
                [ "$host_generation" = "$runner_name" ] \
                    || { log "SKIP $id — guest identity does not match the host generation"; continue; }
            else
                case "$runner_name" in
                    "pulp-auto-ephemeral-${id}"|"pulp-ci-ephemeral-${id}") ;;
                    *) log "SKIP $id — legacy guest identity is not bound to the VMID"; continue ;;
                esac
            fi
            record="$(runner_record "$runner_name" "$host_scope")"; record_status=$?
            [ "$record_status" -eq 0 ] \
                || { log "SKIP $id — exact GitHub runner state is absent or ambiguous"; continue; }
            IFS=$'\t' read -r registration_api rid _ status busy <<< "$record"
            [ "$busy" = false ] && { [ "$status" = online ] || [ "$status" = offline ]; } \
                || { log "SKIP $id — exact GitHub runner is busy or invalid"; continue; }
            registration_present=1
            cleanup_state="idle-listener"
        elif [ "$listener_count" = 0 ]; then
            [[ "$host_generation" =~ ^[A-Za-z0-9._-]+$ ]] \
                || { log "SKIP $id — post-job clone lacks a host generation"; continue; }
            [ "$guest_identity" = "$host_generation" ] \
                || { log "SKIP $id — post-job guest identity does not match the host generation"; continue; }
            runner_record "$runner_name" "$host_scope" >/dev/null; record_status=$?
            [ "$record_status" -eq 1 ] \
                || { log "SKIP $id — post-job registration is present or ambiguous"; continue; }
            cleanup_state="running-post-job"
        else
            log "SKIP $id — guest is not an idle or completed one-job JIT runner"
            continue
        fi
    else
        log "SKIP $id — VM status is neither running nor stopped"
        continue
    fi

    if [ "$DO_IT" = 0 ]; then
        log "WOULD REAP $id — stale ownerless ${cleanup_state} runner $runner_name"
        continue
    fi

    # A clone created by the pre-reaper supervisor can have a generation but
    # cannot prove whether an operator launched it with --keep: that version
    # had no durable keep marker.  Only the updated supervisor's exact
    # generation+scope description proves the clone participates in automatic
    # recovery.  Report legacy candidates, but preserve them in execute mode
    # until an operator explicitly classifies that exact generation.
    case "$host_scope" in
        "orgs/${ORG}"|"repos/${REPO}") ;;
        *) log "SKIP $id — legacy clone lacks an explicit automatic-recovery scope"; continue ;;
    esac

    exec 9>"$VMID_LOCK" || { log "SKIP $id — VMID lock unavailable"; continue; }
    flock -w 300 9 || { log "SKIP $id — VMID lock timed out"; continue; }
    lease_is_live "$id"
    locked_lease_status=$?
    durable_keep_matches "$id" "$host_generation"
    locked_keep_status=$?
    [ -f "$vm_config" ] \
        && [ "$(sha256sum "$vm_config" | awk '{print $1}')" = "$config_digest" ] \
        && [ "$($QM config "$id" 2>/dev/null | sed -n 's/^name: //p')" = "$vm_name" ] \
        && [ "$locked_lease_status" -eq 1 ] \
        && [ "$locked_keep_status" -eq 1 ] \
        || { log "SKIP $id — clone generation, ownership, or keep disposition changed before mutation"; flock -u 9; continue; }

    if [ "$cleanup_state" = idle-listener ]; then
        deregister_fence "$id" "$registration_api" "$rid" "$runner_name" \
            || { flock -u 9; continue; }
        # The registration is gone; nothing below may wait on or delete it.
        registration_present=0
        probe="$(guest_probe "$ip" 2>/dev/null)" \
            || { log "SKIP $id — post-fence guest state cannot be proved"; flock -u 9; continue; }
        # A deregistered listener may already have exited, so the listener count
        # is not part of the proof; no worker and an untouched _work are.
        post_listeners="$(probe_value "$probe" listener_count)"
        [ "$(probe_value "$probe" identity)" = "$runner_name" ] \
            && { [ "$post_listeners" = 0 ] || [ "$post_listeners" = 1 ]; } \
            && [ "$(probe_value "$probe" worker_count)" = 0 ] \
            && [ "$(probe_value "$probe" configurer_count)" = 0 ] \
            && [ "$(probe_value "$probe" work_entries)" = 0 ] \
            || { log "SKIP $id — post-fence guest proof changed"; flock -u 9; continue; }
        log "FENCE $id 4/4 guest is still idle; destroying clone"
    fi
    if [ "$vm_status" = "status: running" ]; then
        "$QM" stop "$id" >/dev/null 2>&1 || true
        stopped=0
        for _ in $(seq 1 60); do
            [ "$($QM status "$id" 2>/dev/null)" = "status: stopped" ] \
                && { stopped=1; break; }
            sleep 2
        done
        [ "$stopped" = 1 ] \
            || { log "SKIP $id — proved clone did not stop"; flock -u 9; continue; }
    fi
    if [ "$registration_present" = 1 ]; then
        for _ in $(seq 1 30); do
            record="$(runner_record "$runner_name" "$host_scope")"; record_status=$?
            [ "$record_status" -eq 0 ] || break
            IFS=$'\t' read -r current_registration_api current_rid _ current_status current_busy <<< "$record"
            [ "$current_registration_api" = "$registration_api" ] \
                && [ "$current_rid" = "$rid" ] \
                && [ "$current_busy" = false ] \
                && [ "$current_status" = offline ] && break
            sleep 2
        done
        if [ "$record_status" -eq 0 ]; then
            [ "$current_registration_api" = "$registration_api" ] \
                && [ "$current_rid" = "$rid" ] \
                && [ "$current_busy" = false ] \
                && [ "$current_status" = offline ] \
                || { log "SKIP $id — fenced runner did not become idle and offline"; flock -u 9; continue; }
            env -u GH_TOKEN -u GITHUB_TOKEN -u GH_ENTERPRISE_TOKEN \
                -u GITHUB_ENTERPRISE_TOKEN HOME=/root \
                "$GH_CLI" api --method DELETE \
                "${registration_api}/actions/runners/${rid}" >/dev/null \
                || { log "SKIP $id — runner deregistration failed"; flock -u 9; continue; }
        elif [ "$record_status" -ne 1 ]; then
            log "SKIP $id — post-stop GitHub state is ambiguous"
            flock -u 9
            continue
        fi
    fi
    log "REAP $id — ${cleanup_state}, offline, deregistered, generation-proved"
    "$QM" destroy "$id" --purge >/dev/null \
        && rm -f -- "${FIREWALL_DIR}/${id}.fw" \
        || log "WARN $id — destroy or firewall cleanup failed"
    flock -u 9
    ssh-keygen -f /root/.ssh/known_hosts -R "$ip" >/dev/null 2>&1 || true
done
