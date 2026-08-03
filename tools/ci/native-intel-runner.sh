#!/bin/bash
# Serve one native Intel macOS job at a time with GitHub's single-job JIT
# configuration. The dedicated labels cannot satisfy Pulp's required ARM64 gate,
# and each job receives a fresh work directory so one branch cannot leave object
# files for another.

set -u
set -o pipefail

REPO="${PULP_NATIVE_INTEL_REPO:-Generous-Corp/pulp}"
LABELS="${PULP_NATIVE_INTEL_LABELS:-self-hosted,macOS,X64,pulp-intel-native,pulp-host-macmini}"
NAME_PREFIX="${PULP_NATIVE_INTEL_NAME_PREFIX:-pulp-intel-macmini}"
RUNNER_GROUP_ID="${PULP_NATIVE_INTEL_RUNNER_GROUP_ID:-}"
POLL_SECONDS="${PULP_NATIVE_INTEL_POLL_SECONDS:-15}"
BUILD_USER="pulp-ci"
BUILD_UID="499"
WORKER="/usr/local/libexec/pulp-native-intel-worker"
TERMINAL_CONFIG_RC=78

CURRENT_RUNNER_NAME=""
ACTIVE_WORKER_PID=""
STOP_REQUESTED=0

note() { printf '[native-intel-runner] %s\n' "$*"; }
fail() { printf '[native-intel-runner] ERROR: %s\n' "$*" >&2; return 1; }

protected_from_build_user() {
    local path="$1" owner mode kind acl
    while :; do
        owner="$(stat -f '%Su' "$path" 2>/dev/null || true)"
        mode="$(stat -f '%OLp' "$path" 2>/dev/null || true)"
        kind="$(stat -f '%HT' "$path" 2>/dev/null || true)"
        [ -n "$owner" ] && [ -n "$mode" ] || {
            fail "cannot inspect account boundary at $path"
            return 1
        }
        [ "$owner" != "$BUILD_USER" ] || {
            fail "build account owns protected controller path $path"
            return 1
        }
        (( (8#$mode & 8#022) == 0 )) || {
            fail "protected controller path is group/world writable: $path"
            return 1
        }
        acl="$(find "$path" -maxdepth 0 -acl -print 2>/dev/null || true)"
        if [ -n "$acl" ] && /bin/ls -lde "$path" 2>/dev/null | \
            tail -n +2 | grep -Eq ' allow .*(write|append|delete|add_|writeattr|writeextattr|writeowner|writesecurity|chown)'; then
            fail "protected controller path has a write-granting ACL: $path"
            return 1
        fi
        [ "$path" != "$1" ] || [ "$kind" = "Regular File" ] || {
            fail "protected controller path must be a regular file: $path"
            return 1
        }
        [ "$path" != / ] || break
        path="$(dirname "$path")"
    done
}

resolve_gh_cli() {
    if [ -n "${PULP_NATIVE_INTEL_GH_CLI:-}" ]; then
        printf '%s\n' "$PULP_NATIVE_INTEL_GH_CLI"
    elif command -v ghapp >/dev/null 2>&1; then
        printf '%s\n' ghapp
    else
        printf '%s\n' gh
    fi
}

GH_CLI="$(resolve_gh_cli)"

validate_labels() {
    local repo_root
    repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
    python3 "$repo_root/tools/scripts/runner_labels.py" \
        --platform macos --labels "$LABELS" --host-tag macmini >/dev/null
}

preflight() {
    local failed=0 controller_path repo_root worker_owner worker_mode build_uid
    [ "$(uname -m)" = x86_64 ] || { fail "host architecture must be x86_64"; failed=1; }
    id "$BUILD_USER" >/dev/null 2>&1 || { fail "dedicated build account $BUILD_USER does not exist"; failed=1; }
    build_uid="$(id -u "$BUILD_USER" 2>/dev/null || true)"
    [ "$build_uid" = "$BUILD_UID" ] || {
        fail "build account $BUILD_USER must have fixed uid $BUILD_UID (found ${build_uid:-missing})"
        failed=1
    }
    [ "$BUILD_USER" != "$(id -un)" ] || { fail "controller and build account must be different users"; failed=1; }
    if id -Gn "$BUILD_USER" 2>/dev/null | tr ' ' '\n' | grep -qx admin; then
        fail "build account $BUILD_USER must not be an administrator"
        failed=1
    fi
    [ -x "$WORKER" ] || { fail "root-owned worker missing at $WORKER"; failed=1; }
    if [ -e "$WORKER" ]; then
        worker_owner="$(stat -f '%Su' "$WORKER" 2>/dev/null || true)"
        worker_mode="$(stat -f '%OLp' "$WORKER" 2>/dev/null || true)"
        [ "$worker_owner" = root ] || { fail "$WORKER must be owned by root"; failed=1; }
        if [ -z "$worker_mode" ] || (( (8#$worker_mode & 8#022) != 0 )); then
            fail "$WORKER must not be group/world writable"
            failed=1
        fi
    fi
    protected_from_build_user "$WORKER" || failed=1
    controller_path="$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")"
    repo_root="$(cd "$(dirname "$0")/../.." && pwd -P)"
    protected_from_build_user "$controller_path" || failed=1
    protected_from_build_user "$repo_root/tools/scripts/runner_labels.py" || failed=1
    protected_from_build_user "$repo_root/tools/ci/verify_native_intel_runner_group.py" || failed=1
    command -v "$GH_CLI" >/dev/null 2>&1 || { fail "$GH_CLI is not on PATH"; failed=1; }
    case "$RUNNER_GROUP_ID" in
        ''|*[!0-9]*)
            fail "PULP_NATIVE_INTEL_RUNNER_GROUP_ID must name the dedicated restricted runner group"
            failed=1
            ;;
        1)
            fail "runner group 1 is the default group; a dedicated workflow-restricted group is required"
            failed=1
            ;;
    esac
    validate_labels || { fail "runner labels do not satisfy a declared macOS lane"; failed=1; }
    [ "$failed" -eq 0 ] || return 1
    /usr/bin/sudo -n "$WORKER" --check || {
        fail "isolated build-account preflight failed"
        return 1
    }
    "$GH_CLI" auth status >/dev/null || {
        fail "$GH_CLI is not authenticated"
        return 1
    }
    python3 "$repo_root/tools/ci/verify_native_intel_runner_group.py" \
        --gh "$GH_CLI" --repo "$REPO" --group-id "$RUNNER_GROUP_ID" || {
        fail "the native Intel runner group policy is not fail-closed"
        return 1
    }
    note "preflight passed: arch=x86_64 labels=$LABELS build_user=$BUILD_USER"
}

runner_id_and_busy() {
    [ -n "$CURRENT_RUNNER_NAME" ] || return 0
    "$GH_CLI" api "repos/$REPO/actions/runners?per_page=100" --paginate \
        --jq ".runners[] | select(.name == \"$CURRENT_RUNNER_NAME\") | \"\\(.id) \\(.busy)\"" \
        2>/dev/null | head -1
}

remove_idle_registration() {
    local row runner_id busy
    row="$(runner_id_and_busy || true)"
    [ -n "$row" ] || return 0
    runner_id="${row%% *}"
    busy="${row#* }"
    if [ "$busy" = true ]; then
        note "leaving busy runner registration intact: $CURRENT_RUNNER_NAME"
        return 0
    fi
    "$GH_CLI" api -X DELETE "repos/$REPO/actions/runners/$runner_id" >/dev/null 2>&1 || true
}

request_stop() {
    STOP_REQUESTED=1
    if [ -n "$ACTIVE_WORKER_PID" ] && kill -0 "$ACTIVE_WORKER_PID" 2>/dev/null; then
        note "forwarding stop request to active isolated worker"
        kill -TERM "$ACTIVE_WORKER_PID" 2>/dev/null || true
    fi
    remove_idle_registration
}
trap request_stop INT TERM
trap remove_idle_registration EXIT

clean_workspace() {
    /usr/bin/sudo -n "$WORKER" --clean || return "$TERMINAL_CONFIG_RC"
}

mint_jit() {
    local label_args=() label
    IFS=',' read -r -a split_labels <<< "$LABELS"
    for label in "${split_labels[@]}"; do
        label_args+=(--raw-field "labels[]=$label")
    done
    "$GH_CLI" api -X POST "repos/$REPO/actions/runners/generate-jitconfig" \
        --raw-field "name=$CURRENT_RUNNER_NAME" \
        --field "runner_group_id=$RUNNER_GROUP_ID" \
        --raw-field "work_folder=_work-native-intel" \
        "${label_args[@]}" \
        --jq '.encoded_jit_config'
}

wait_for_active_worker() {
    local rc=0
    while :; do
        wait "$ACTIVE_WORKER_PID" || rc=$?
        # A trapped controller signal interrupts `wait` before sudo necessarily
        # finishes forwarding it. Do not race a second privileged cleanup
        # against the still-live worker; wait again until the child is reaped.
        kill -0 "$ACTIVE_WORKER_PID" 2>/dev/null || break
    done
    return "$rc"
}

run_one() {
    local sequence="$1" jit rc=0
    clean_workspace || return "$TERMINAL_CONFIG_RC"
    CURRENT_RUNNER_NAME="$NAME_PREFIX-$(date -u +%Y%m%d%H%M%S)-$$-$sequence"
    note "minting one-job runner $CURRENT_RUNNER_NAME"
    jit="$(mint_jit)" || { fail "JIT configuration mint failed"; return 1; }
    [ -n "$jit" ] || { fail "GitHub returned an empty JIT configuration"; return 1; }

    printf '%s\n' "$jit" | /usr/bin/sudo -n "$WORKER" --run &
    ACTIVE_WORKER_PID=$!
    if [ "$STOP_REQUESTED" -ne 0 ]; then
        kill -TERM "$ACTIVE_WORKER_PID" 2>/dev/null || true
    fi
    wait_for_active_worker || rc=$?
    ACTIVE_WORKER_PID=""
    jit=""
    remove_idle_registration
    CURRENT_RUNNER_NAME=""
    if ! clean_workspace; then
        rc="$TERMINAL_CONFIG_RC"
    fi
    return "$rc"
}

usage() {
    printf 'usage: %s --check | --once | --loop\n' "${0##*/}"
}

hold_lane_offline() {
    fail "$1; holding lane offline"
    while [ "$STOP_REQUESTED" -eq 0 ]; do
        sleep 3600
    done
    return "$TERMINAL_CONFIG_RC"
}

mode="${1:-}"
case "$mode" in
    --check)
        preflight
        ;;
    --once)
        preflight || exit 1
        run_one 1
        ;;
    --loop)
        if ! preflight; then
            hold_lane_offline "startup preflight failed"
            exit $?
        fi
        sequence=0
        while [ "$STOP_REQUESTED" -eq 0 ]; do
            sequence=$((sequence + 1))
            run_one "$sequence"
            rc=$?
            if [ "$rc" -eq "$TERMINAL_CONFIG_RC" ]; then
                hold_lane_offline "worker integrity/configuration validation failed"
                exit $?
            fi
            if [ "$rc" -ne 0 ]; then
                note "runner cycle was interrupted; starting a new identity in ${POLL_SECONDS}s"
            fi
            [ "$STOP_REQUESTED" -ne 0 ] || sleep "$POLL_SECONDS"
        done
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
