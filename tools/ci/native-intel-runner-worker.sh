#!/bin/bash
# Root-owned lifecycle shim for the unprivileged native Intel build account.
# The credentialed controller may invoke only these fixed operations via sudo.

set -u
set -o pipefail
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH

BUILD_USER="pulp-ci"
BUILD_UID="499"
BUILD_GROUP="staff"
BUILD_GID="20"
JOB_ROOT="/private/var/tmp/pulp-native-intel-job"
JOB_HOME="$JOB_ROOT/home"
JOB_TMP="$JOB_ROOT/tmp"
RUNNER_DIR="$JOB_ROOT/runner"
WORK_FOLDER="_work-native-intel"
TRUST_ROOT="/usr/local/share/pulp-native-intel"
RUNNER_GOLDEN="$TRUST_ROOT/actions-runner-mini"
TOOLS_DIR="$TRUST_ROOT/bin"
CCACHE_DIR="$TRUST_ROOT/ccache"
XCODE_DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer"
XCODE_BUNDLE="/Applications/Xcode.app"
WORKER_INSTALL="/usr/local/libexec/pulp-native-intel-worker"
TERMINAL_CONFIG_RC=78
ACTIVE_JOB_PID=""

fail() { printf '[native-intel-worker] ERROR: %s\n' "$*" >&2; return 1; }

assert_worker_install() {
    local actual owner mode kind path
    actual="$(cd "$(dirname "$0")" 2>/dev/null && pwd -P)/$(basename "$0")"
    [ "$actual" = "$WORKER_INSTALL" ] || {
        fail "worker must run from fixed install path $WORKER_INSTALL"
        return 1
    }
    owner="$(stat -f '%Su' "$actual" 2>/dev/null || true)"
    mode="$(stat -f '%OLp' "$actual" 2>/dev/null || true)"
    kind="$(stat -f '%HT' "$actual" 2>/dev/null || true)"
    [ "$owner" = root ] && [ "$kind" = "Regular File" ] && [ -n "$mode" ] && \
        (( (8#$mode & 8#022) == 0 )) || {
        fail "$WORKER_INSTALL must be a root-owned, non-writable regular file"
        return 1
    }
    path="$actual"
    while :; do
        find "$path" -maxdepth 0 -acl -print -quit | grep -q . && {
            fail "worker path component has an extended ACL: $path"
            return 1
        }
        assert_not_writable_by_build_user "$path" || return 1
        [ "$path" != / ] || break
        path="$(dirname "$path")"
    done
}

assert_root_boundary() {
    [ "$(id -u)" -eq 0 ] || {
        fail "must run as root through the fixed sudoers rule"
        return 1
    }
    id "$BUILD_USER" >/dev/null 2>&1 || {
        fail "dedicated build account $BUILD_USER does not exist"
        return 1
    }
    [ "$(id -u "$BUILD_USER")" = "$BUILD_UID" ] || {
        fail "$BUILD_USER must have fixed uid $BUILD_UID"
        return 1
    }
    [ "$(id -g "$BUILD_USER")" = "$BUILD_GID" ] || {
        fail "$BUILD_USER must have primary gid $BUILD_GID ($BUILD_GROUP)"
        return 1
    }
    [ "$(id -gn "$BUILD_USER")" = "$BUILD_GROUP" ] || {
        fail "$BUILD_USER primary group must be $BUILD_GROUP"
        return 1
    }
    if id -Gn "$BUILD_USER" | tr ' ' '\n' | grep -qx admin; then
        fail "$BUILD_USER must not be an administrator"
        return 1
    fi
}

dscl_value() {
    /usr/bin/dscl . -read "/Users/$BUILD_USER" "$1" 2>/dev/null | \
        sed -e "s/^$1: //"
}

assert_service_identity() {
    local home shell hidden empty_owner empty_mode
    home="$(dscl_value NFSHomeDirectory)"
    shell="$(dscl_value UserShell)"
    [ "$home" = /var/empty ] || {
        fail "$BUILD_USER NFSHomeDirectory must be /var/empty"
        return 1
    }
    [ "$shell" = /usr/bin/false ] || {
        fail "$BUILD_USER login shell must be /usr/bin/false"
        return 1
    }
    hidden="$(dscl_value IsHidden)"
    [ "$hidden" = 1 ] || {
        fail "$BUILD_USER must be a hidden service account"
        return 1
    }
    /usr/bin/dscl . -read "/Users/$BUILD_USER" AuthenticationAuthority 2>/dev/null | \
        grep -q DisabledUser || {
        fail "$BUILD_USER must be disabled for interactive authentication"
        return 1
    }
    empty_owner="$(stat -f '%Su' /var/empty 2>/dev/null || true)"
    empty_mode="$(stat -f '%OLp' /var/empty 2>/dev/null || true)"
    [ "$empty_owner" = root ] && [ -n "$empty_mode" ] && \
        (( (8#$empty_mode & 8#022) == 0 )) || {
        fail "/var/empty must be root-owned and not group/world writable"
        return 1
    }
    ! /usr/bin/sudo -n -H -u "$BUILD_USER" /bin/test -w /var/empty || {
        fail "$BUILD_USER must not be able to write its real home /var/empty"
        return 1
    }
}

assert_no_crontab() {
    ! /usr/bin/crontab -u "$BUILD_USER" -l >/dev/null 2>&1 || {
        fail "$BUILD_USER must not have an installed crontab"
        return 1
    }
}

assert_at_scheduler_disabled() {
    [ "$(/usr/bin/plutil -extract Disabled raw -o - \
        /System/Library/LaunchDaemons/com.apple.atrun.plist 2>/dev/null)" = true ] || {
        fail "com.apple.atrun must remain disabled in the system launchd plist"
        return 1
    }
    ! /bin/launchctl print system/com.apple.atrun >/dev/null 2>&1 || {
        fail "com.apple.atrun must not be loaded"
        return 1
    }
}

clean_at_jobs() {
    local jobs
    jobs="$(/usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/atq)" || {
        fail "could not inspect queued at jobs for $BUILD_USER"
        return 1
    }
    printf '%s\n' "$jobs" | awk '$1 ~ /^[0-9]+$/ { print $1 }' | \
        while IFS= read -r job_id; do
            [ -n "$job_id" ] || continue
            /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/atrm "$job_id" || exit 1
        done || {
        fail "could not remove queued at jobs for $BUILD_USER"
        return 1
    }
    jobs="$(/usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/atq)" || {
        fail "could not verify queued at jobs are absent for $BUILD_USER"
        return 1
    }
    [ -z "$jobs" ] || {
        fail "$BUILD_USER still has queued at jobs"
        return 1
    }
}

assert_not_writable_by_build_user() {
    local path="$1"
    ! /usr/bin/sudo -n -H -u "$BUILD_USER" /bin/test -w "$path" || {
        fail "$BUILD_USER can write protected path $path"
        return 1
    }
}

assert_contained_symlinks() {
    local path="$1" trust_root="$2" link resolved
    while IFS= read -r -d '' link; do
        resolved="$(/bin/realpath "$link" 2>/dev/null || true)"
        case "$resolved" in
            "$trust_root"/*) ;;
            *)
                fail "$link escapes trusted tree $trust_root"
                return 1
                ;;
        esac
    done < <(find "$path" -type l -print0)
}

assert_immutable_tree() {
    local path="$1" symlink_root="${2:-$1}" owner
    [ -d "$path" ] || { fail "trusted tree missing at $path"; return 1; }
    owner="$(stat -f '%Su' "$path" 2>/dev/null || true)"
    [ "$owner" = root ] || { fail "$path must be owned by root"; return 1; }
    if find "$path" ! -user root -print -quit | grep -q .; then
        fail "$path contains an entry not owned by root"
        return 1
    fi
    if find "$path" \( -perm -0020 -o -perm -0002 \) -print -quit | grep -q .; then
        fail "$path contains a group/world-writable entry"
        return 1
    fi
    if find "$path" -acl -print -quit | grep -q .; then
        fail "$path contains an extended ACL"
        return 1
    fi
    assert_contained_symlinks "$path" "$symlink_root" || return 1
    assert_not_writable_by_build_user "$path"
}

stop_build_processes() {
    local uid attempts=0
    uid="$(id -u "$BUILD_USER")" || return 1
    /usr/bin/pkill -KILL -u "$uid" >/dev/null 2>&1 || true
    while /usr/bin/pgrep -u "$uid" >/dev/null 2>&1; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 20 ] || {
            fail "could not stop every process owned by uid $uid"
            return 1
        }
        sleep 0.1
    done
}

clean_uid_state() {
    local uid root
    uid="$(id -u "$BUILD_USER")" || return 1
    # This fixed service uid owns no durable host state. Cover macOS's shared
    # writable roots as well as temp state without traversing TCC-protected user
    # homes. A user's write-only Public Drop Box is another persistence surface.
    for root in /private/tmp /private/var/tmp /private/var/folders \
        /Users/Shared /Library/Caches /Library/Preferences/Audio/Data \
        /Users/*/Public/"Drop Box"; do
        [ -d "$root" ] || continue
        find "$root" -xdev -user "$uid" -depth -delete 2>/dev/null || {
            fail "could not remove uid-$uid state below $root"
            return 1
        }
    done
}

clean_job_root() {
    case "$JOB_ROOT" in
        "/private/var/tmp/pulp-native-intel-job")
            stop_build_processes || return 1
            /usr/bin/crontab -u "$BUILD_USER" -r >/dev/null 2>&1 || true
            clean_at_jobs || return 1
            assert_at_scheduler_disabled || return 1
            # Catch a cron process already dispatched while the table was
            # being removed, then prove no workflow persistence remains.
            stop_build_processes || return 1
            assert_no_crontab || return 1
            rm -rf "$JOB_ROOT" || return 1
            clean_uid_state || return 1
            ;;
        *)
            fail "refusing to clean unexpected job root: $JOB_ROOT"
            return 1
            ;;
    esac
}

cleanup_on_signal() {
    local rc="$1"
    trap - EXIT INT TERM
    if [ -n "$ACTIVE_JOB_PID" ] && kill -0 "$ACTIVE_JOB_PID" 2>/dev/null; then
        kill -TERM "$ACTIVE_JOB_PID" 2>/dev/null || true
    fi
    clean_job_root || true
    if [ -n "$ACTIVE_JOB_PID" ]; then
        while kill -0 "$ACTIVE_JOB_PID" 2>/dev/null; do
            wait "$ACTIVE_JOB_PID" 2>/dev/null || true
        done
    fi
    exit "$rc"
}

probe_build_environment() {
    local tool
    for tool in cmake ccache ninja git-lfs; do
        /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
            HOME=/var/empty PATH="$TOOLS_DIR:/usr/bin:/bin:/usr/sbin:/sbin" \
            /bin/test -x "$TOOLS_DIR/$tool" || {
            fail "$tool is not executable by $BUILD_USER"
            return 1
        }
    done
    /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
        HOME=/var/empty PATH="$TOOLS_DIR:/usr/bin:/bin:/usr/sbin:/sbin" \
        "$TOOLS_DIR/cmake" --version >/dev/null || return 1
    /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
        HOME=/var/empty PATH="$TOOLS_DIR:/usr/bin:/bin:/usr/sbin:/sbin" \
        CCACHE_DIR="$CCACHE_DIR" CCACHE_READONLY=1 CCACHE_NODEPEND=1 \
        "$TOOLS_DIR/ccache" --show-config >/dev/null || return 1
    /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
        HOME=/var/empty PATH="$TOOLS_DIR:/usr/bin:/bin:/usr/sbin:/sbin" \
        "$TOOLS_DIR/ninja" --version >/dev/null || return 1
    /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
        HOME=/var/empty PATH="$TOOLS_DIR:/usr/bin:/bin:/usr/sbin:/sbin" \
        "$TOOLS_DIR/git-lfs" version >/dev/null || return 1
    /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
        HOME=/var/empty PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        /bin/sh -c '
            first=$(/usr/bin/find "$1" -mindepth 1 -print -quit) || exit 1
            [ -n "$first" ] || exit 1
            /usr/bin/find "$1" -type f -exec /bin/sh -c '\''
                for entry do
                    [ -r "$entry" ] || exit 1
                done
            '\'' sh {} +
        ' sh "$CCACHE_DIR" || {
        fail "warm ccache is empty or not recursively readable by $BUILD_USER"
        return 1
    }
}

check() {
    local failed=0
    assert_root_boundary || failed=1
    assert_service_identity || failed=1
    assert_worker_install || failed=1
    assert_no_crontab || failed=1
    assert_at_scheduler_disabled || failed=1
    assert_immutable_tree "$TRUST_ROOT" "$TRUST_ROOT" || failed=1
    # The runner's own links must remain inside the runner golden even though
    # trusted tool links may target sibling bundles elsewhere in TRUST_ROOT.
    assert_contained_symlinks "$RUNNER_GOLDEN" "$RUNNER_GOLDEN" || failed=1
    # Xcode legitimately contains framework symlinks. Root ownership and no
    # group/world-writable entries keep the build uid from retargeting them.
    assert_immutable_tree "$XCODE_BUNDLE" "$XCODE_BUNDLE" || failed=1
    [ -x "$RUNNER_GOLDEN/run.sh" ] || { fail "golden runner has no run.sh"; failed=1; }
    [ -x "$XCODE_DEVELOPER_DIR/usr/bin/xcodebuild" ] || {
        fail "shared full Xcode missing at $XCODE_DEVELOPER_DIR"; failed=1;
    }
    for tool in cmake ccache ninja git-lfs; do
        [ -x "$TOOLS_DIR/$tool" ] || { fail "$tool missing from trusted tools"; failed=1; }
    done
    [ "$failed" -eq 0 ] || return 1
    probe_build_environment || {
        fail "trusted tools or warm ccache are unusable by $BUILD_USER"
        return 1
    }
    /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
        HOME=/var/empty PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        DEVELOPER_DIR="$XCODE_DEVELOPER_DIR" \
        /usr/bin/codesign --verify --deep --strict "$XCODE_BUNDLE" >/dev/null 2>&1 || {
        fail "the shared Xcode signature is invalid"; return 1;
    }
    /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
        HOME=/var/empty PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        DEVELOPER_DIR="$XCODE_DEVELOPER_DIR" \
        /usr/sbin/spctl --assess --type execute "$XCODE_BUNDLE" >/dev/null 2>&1 || {
        fail "the shared Xcode bundle is not accepted by Gatekeeper"; return 1;
    }
    /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
        HOME=/var/empty PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        DEVELOPER_DIR="$XCODE_DEVELOPER_DIR" \
        "$XCODE_DEVELOPER_DIR/usr/bin/xcodebuild" -license status >/dev/null 2>&1 || {
        fail "the Xcode license has not been accepted"; return 1;
    }
    /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
        HOME=/var/empty PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        DEVELOPER_DIR="$XCODE_DEVELOPER_DIR" \
        /usr/bin/xcrun clang --version >/dev/null || {
        fail "the selected Xcode toolchain cannot run clang"; return 1;
    }
    /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
        HOME=/var/empty PATH=/usr/bin:/bin:/usr/sbin:/sbin \
        DEVELOPER_DIR="$XCODE_DEVELOPER_DIR" \
        "$XCODE_DEVELOPER_DIR/usr/bin/xcodebuild" -version >/dev/null || {
        fail "the selected Xcode toolchain cannot run xcodebuild"; return 1;
    }
}

prepare_fresh_runner() {
    clean_job_root || return 1
    umask 0077
    mkdir -p "$JOB_HOME" "$JOB_TMP" "$JOB_ROOT/ccache-tmp"
    /usr/bin/ditto "$RUNNER_GOLDEN" "$RUNNER_DIR" || return 1
    chown -R "$BUILD_USER:$BUILD_GROUP" "$JOB_ROOT" || return 1
    chmod 0700 "$JOB_ROOT" "$JOB_HOME" "$JOB_TMP" "$JOB_ROOT/ccache-tmp" || return 1
}

run_one() {
    local jit="" rc=0
    check || return "$TERMINAL_CONFIG_RC"
    IFS= read -r jit
    [ -n "$jit" ] || { fail "empty JIT configuration"; return 1; }
    case "$jit" in
        *[!A-Za-z0-9_+=/-]*) fail "invalid JIT configuration encoding"; return 1 ;;
    esac
    prepare_fresh_runner || return 1
    trap 'clean_job_root || true' EXIT
    trap 'cleanup_on_signal 130' INT
    trap 'cleanup_on_signal 143' TERM
    cd "$RUNNER_DIR" || return 1
    /usr/bin/sudo -n -H -u "$BUILD_USER" /usr/bin/env -i \
        HOME="$JOB_HOME" \
        USER="$BUILD_USER" \
        LOGNAME="$BUILD_USER" \
        TMPDIR="$JOB_TMP/" \
        PATH="$TOOLS_DIR:/usr/bin:/bin:/usr/sbin:/sbin" \
        DEVELOPER_DIR="$XCODE_DEVELOPER_DIR" \
        CCACHE_DIR="$CCACHE_DIR" \
        CCACHE_READONLY=1 \
        CCACHE_NODEPEND=1 \
        CCACHE_TEMPDIR="$JOB_ROOT/ccache-tmp" \
        ./run.sh --jitconfig "$jit" &
    ACTIVE_JOB_PID=$!
    wait "$ACTIVE_JOB_PID" || rc=$?
    while kill -0 "$ACTIVE_JOB_PID" 2>/dev/null; do
        wait "$ACTIVE_JOB_PID" || rc=$?
    done
    ACTIVE_JOB_PID=""
    jit=""
    clean_job_root || rc=1
    trap - EXIT INT TERM
    return "$rc"
}

case "${1:-}" in
    --check) check ;;
    --clean) assert_root_boundary && assert_service_identity && \
        assert_worker_install && clean_job_root ;;
    --run) run_one ;;
    *) fail "usage: ${0##*/} --check | --clean | --run"; exit 2 ;;
esac
