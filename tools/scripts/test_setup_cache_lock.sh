#!/usr/bin/env bash
# test_setup_cache_lock.sh — setup.sh's shared source cache survives a killed run.
#
# A priming step that is killed leaves two things behind: its `.<dir>.lock`
# directory, which the next run used to wait on forever, and a half-populated
# cache directory, which the next run used to treat as ready (a clone killed
# before its first checkout leaves .git with an empty index and no files, and
# configure then fails on every Catch2 target). These tests kill real priming
# runs and check that the next run reclaims the lock and re-fetches the cache,
# and that a lock held by a live run is never taken. Runs against throwaway
# file:// repositories under Git Bash on Windows as well as macOS and Linux.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SETUP_SH="$REPO_ROOT/setup.sh"

unset PULP_SHARED_FETCHCONTENT_SOURCE_DIR
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_SYSTEM=/dev/null

PASS=0
FAIL=0
ok()    { echo "  ✓ $*"; PASS=$((PASS + 1)); }
bad()   { echo "  ✗ $*"; FAIL=$((FAIL + 1)); }
check() { if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (want '$2', got '$1')"; fi; }

load_setup_lib() {
    set --
    # shellcheck disable=SC1090
    PULP_SETUP_LIB_ONLY=1 . "$SETUP_SH"
    set +e
}

make_upstream() {
    local dir="$1"
    git init -q "$dir"
    git -C "$dir" config user.email t@t.t
    git -C "$dir" config user.name t
    git -C "$dir" config uploadpack.allowFilter true
    git -C "$dir" config uploadpack.allowAnySHA1InWant true
    printf 'license\n' > "$dir/LICENSE.txt"
    printf 'readme\n' > "$dir/README.md"
    git -C "$dir" add LICENSE.txt README.md
    git -C "$dir" commit -qm v1
    git -C "$dir" tag v1
}

# A pid that is certainly not running: a child that has already exited.
dead_pid() {
    sh -c 'exit 0' &
    local pid=$!
    wait "$pid" 2>/dev/null
    echo "$pid"
}

# Run a command in the background for at most $1 seconds; prints its exit code,
# or "timeout" (and kills it) when it is still running at the deadline. The
# command gets its own process group (set -m) so the kill reaches the priming
# subshell setup.sh starts, not just the wrapper; an orphan left waiting would
# hold this function's output open and hang the caller.
run_bounded() {
    local seconds="$1"; shift
    set -m
    "$@" &
    local pid=$! waited=0
    set +m
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge "$((seconds * 10))" ]; then
            kill -9 -- "-$pid" 2>/dev/null || kill -9 "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            echo timeout
            return
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
    wait "$pid"
    echo "$?"
}

prime() {
    ensure_shared_git_source "Dep" "file://$UPSTREAM" "v1" "dep-v1" > "$OUT" 2>&1
}

echo "== a lock left by a killed priming run is reclaimed, not waited on"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    UPSTREAM="$tmp/upstream"; OUT="$tmp/out"
    make_upstream "$UPSTREAM"
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    mkdir -p "$FETCHCONTENT_CACHE_ROOT/.dep-v1.lock"
    printf 'pid=%s\nhost=%s\nstarted=%s\n' "$(dead_pid)" "$(source_cache_host)" "$(date +%s)" \
        > "$FETCHCONTENT_CACHE_ROOT/.dep-v1.lock/owner"

    check "$(run_bounded 30 prime)" "0" "priming finishes instead of waiting forever"
    check "$(grep -c 'Reclaiming the shared Dep source cache lock because its owner' "$OUT")" "1" \
        "the reclaim is announced, naming the dead owner"
    check "$([ -f "$FETCHCONTENT_CACHE_ROOT/dep-v1/LICENSE.txt" ] && echo yes || echo no)" "yes" \
        "the cache is populated"
    check "$([ -e "$FETCHCONTENT_CACHE_ROOT/.dep-v1.lock" ] && echo held || echo released)" "released" \
        "the lock is released afterwards"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== a lock held by a live run is never reclaimed"
(
    load_setup_lib
    tmp="$(mktemp -d)"
    UPSTREAM="$tmp/upstream"; OUT="$tmp/out"
    make_upstream "$UPSTREAM"
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    lock="$FETCHCONTENT_CACHE_ROOT/.dep-v1.lock"
    mkdir -p "$lock"
    sleep 60 &
    holder=$!
    trap 'kill "$holder" 2>/dev/null; wait "$holder" 2>/dev/null; rm -rf "$tmp"' EXIT
    owner="$(printf 'pid=%s\nhost=%s\nstarted=%s' "$holder" "$(source_cache_host)" "$(date +%s)")"
    printf '%s\n' "$owner" > "$lock/owner"

    check "$(run_bounded 4 prime)" "timeout" "priming waits while the owner is alive"
    check "$(grep -c 'Waiting for shared Dep source cache lock' "$OUT")" "1" "it says it is waiting"
    check "$(grep -c 'Reclaiming' "$OUT")" "0" "it does not reclaim a live lock"
    check "$(cat "$lock/owner")" "$owner" "the live owner's lock is untouched"
    check "$([ -e "$FETCHCONTENT_CACHE_ROOT/dep-v1" ] && echo touched || echo untouched)" "untouched" \
        "the cache is not written under someone else's lock"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== a live owner on this host keeps its lock past the age bound"
(
    load_setup_lib
    tmp="$(mktemp -d)"
    UPSTREAM="$tmp/upstream"; OUT="$tmp/out"
    make_upstream "$UPSTREAM"
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    SOURCE_CACHE_LOCK_STALE_SECONDS=1
    lock="$FETCHCONTENT_CACHE_ROOT/.dep-v1.lock"
    mkdir -p "$lock"
    sleep 60 &
    holder=$!
    trap 'kill "$holder" 2>/dev/null; wait "$holder" 2>/dev/null; rm -rf "$tmp"' EXIT
    owner="$(printf 'pid=%s\nhost=%s\nstarted=%s' "$holder" "$(source_cache_host)" 1)"
    printf '%s\n' "$owner" > "$lock/owner"

    check "$(run_bounded 4 prime)" "timeout" "a slow live owner is still waited on"
    check "$(grep -c 'Reclaiming' "$OUT")" "0" "age alone never takes a live same-host lock"
    check "$(cat "$lock/owner")" "$owner" "the live owner's lock is untouched"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== an owner on another host is reclaimed by age"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    UPSTREAM="$tmp/upstream"; OUT="$tmp/out"
    make_upstream "$UPSTREAM"
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    SOURCE_CACHE_LOCK_STALE_SECONDS=60
    mkdir -p "$FETCHCONTENT_CACHE_ROOT/.dep-v1.lock"
    printf 'pid=%s\nhost=%s\nstarted=%s\n' "$$" "some-other-host" 1 \
        > "$FETCHCONTENT_CACHE_ROOT/.dep-v1.lock/owner"
    check "$(run_bounded 30 prime)" "0" "priming finishes"
    check "$(grep -c 'liveness cannot be checked here' "$OUT")" "1" \
        "the reclaim says liveness was unknowable"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== an owner-less lock is reclaimed only once it is older than the bound"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    UPSTREAM="$tmp/upstream"; OUT="$tmp/out"
    make_upstream "$UPSTREAM"
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    SOURCE_CACHE_LOCK_STALE_SECONDS=120
    lock="$FETCHCONTENT_CACHE_ROOT/.dep-v1.lock"
    mkdir -p "$lock"
    check "$(run_bounded 4 prime)" "timeout" "a fresh owner-less lock is waited on"
    touch -t 202001010000 "$lock"
    check "$(run_bounded 30 prime)" "0" "an old owner-less lock is reclaimed"
    check "$(grep -c 'has no owner record and is older than 120s' "$OUT")" "1" \
        "the reclaim names the age bound"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== a priming run killed mid-clone leaves a cache the next run re-fetches"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    UPSTREAM="$tmp/upstream"; OUT="$tmp/out"
    make_upstream "$UPSTREAM"
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    target="$FETCHCONTENT_CACHE_ROOT/dep-v1"

    # A git that clones without checking out and then hangs, which is where the
    # real killed run stopped: .git exists, the index is empty, no files.
    real_git="$(command -v git)"
    mkdir -p "$tmp/bin"
    cat > "$tmp/bin/git" <<EOF
#!/usr/bin/env bash
if [ "\${1:-}" = clone ]; then
    "$real_git" clone --no-checkout "\${@:2}" || exit 1
    # Stand in for a checkout in progress: git holds index.lock while it writes.
    : > "\${@: -1}/.git/index.lock"
    : > "$tmp/clone-landed"
    sleep 600
fi
exec "$real_git" "\$@"
EOF
    chmod +x "$tmp/bin/git"
    set -m
    (
        PATH="$tmp/bin:$PATH"
        prime
    ) &
    victim=$!
    set +m
    for _ in $(seq 1 300); do [ -e "$tmp/clone-landed" ] && break; sleep 0.1; done
    # Kill the whole priming run the way a cancelled job or a host reboot does:
    # no EXIT trap runs, so the lock and the half-cloned cache stay behind.
    kill -9 -- "-$victim" 2>/dev/null || kill -9 "$victim" 2>/dev/null
    wait "$victim" 2>/dev/null

    check "$([ -e "$tmp/clone-landed" ] && echo yes || echo no)" "yes" \
        "the killed run got as far as the clone (test precondition)"
    check "$([ -d "$target/.git" ] && echo yes || echo no)" "yes" \
        "the killed run left a .git behind (test precondition)"
    check "$([ -f "$target/LICENSE.txt" ] && echo yes || echo no)" "no" \
        "the killed run left no files behind (test precondition)"
    check "$([ -d "$FETCHCONTENT_CACHE_ROOT/.dep-v1.lock" ] && echo yes || echo no)" "yes" \
        "the killed run left its lock behind (test precondition)"
    check "$([ -e "$FETCHCONTENT_CACHE_ROOT/.dep-v1.complete" ] && echo yes || echo no)" "no" \
        "the killed run wrote no completion marker"

    check "$(run_bounded 30 prime)" "0" "the next run succeeds"
    check "$(grep -c 'is incomplete; re-fetching it' "$OUT")" "1" \
        "the half-cloned cache is discarded rather than trusted"
    check "$(cat "$target/LICENSE.txt" 2>/dev/null)" "license" "the cache now holds the files"
    check "$(sed -n 's/^ref=//p' "$FETCHCONTENT_CACHE_ROOT/.dep-v1.complete" 2>/dev/null)" "v1" \
        "the completion marker is written for the primed ref"
    check "$(ls -d "$FETCHCONTENT_CACHE_ROOT"/dep-v1.incomplete.* 2>/dev/null | wc -l | tr -d ' ')" "0" \
        "the discarded copy is cleaned up"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== a half-cloned cache with no lock left behind is still not trusted"
(
    # The completion check on its own, without a stale lock in the way: this is
    # the state that made configure fail on every Catch2 target.
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    UPSTREAM="$tmp/upstream"; OUT="$tmp/out"
    make_upstream "$UPSTREAM"
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    mkdir -p "$FETCHCONTENT_CACHE_ROOT"
    git clone -q --no-checkout --filter=blob:none "file://$UPSTREAM" "$FETCHCONTENT_CACHE_ROOT/dep-v1"
    # Killed while checking out: git's index.lock is left behind and no index
    # was ever written. Every later checkout then fails on the lock, and a
    # completeness check that reads the (missing) index sees nothing missing.
    : > "$FETCHCONTENT_CACHE_ROOT/dep-v1/.git/index.lock"
    check "$(run_bounded 30 prime)" "0" "priming succeeds"
    check "$(cat "$FETCHCONTENT_CACHE_ROOT/dep-v1/LICENSE.txt" 2>/dev/null)" "license" \
        "the cache holds the files, not an empty checkout"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== a complete cache from before completion markers is adopted, not re-fetched"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    UPSTREAM="$tmp/upstream"; OUT="$tmp/out"
    make_upstream "$UPSTREAM"
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    mkdir -p "$FETCHCONTENT_CACHE_ROOT"
    git clone -q "file://$UPSTREAM" "$FETCHCONTENT_CACHE_ROOT/dep-v1"
    git -C "$FETCHCONTENT_CACHE_ROOT/dep-v1" checkout -q --detach v1
    inode_before="$(ls -di "$FETCHCONTENT_CACHE_ROOT/dep-v1" | awk '{print $1}')"

    check "$(run_bounded 30 prime)" "0" "priming succeeds"
    check "$(grep -c 're-fetching' "$OUT")" "0" "a verified legacy cache is not discarded"
    check "$(ls -di "$FETCHCONTENT_CACHE_ROOT/dep-v1" | awk '{print $1}')" "$inode_before" \
        "the same directory is kept in place"
    check "$([ -e "$FETCHCONTENT_CACHE_ROOT/.dep-v1.complete" ] && echo yes || echo no)" "yes" \
        "it gains a completion marker"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== an archive cache without a completion marker is re-fetched"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    mkdir -p "$tmp/src/lib" "$FETCHCONTENT_CACHE_ROOT/rt-v1"
    printf 'runtime\n' > "$tmp/src/lib/runtime.dll"
    # Git for Windows ships unzip but not zip.
    if command -v zip >/dev/null 2>&1; then
        (cd "$tmp/src" && zip -qr "$tmp/rt.zip" lib)
    else
        (cd "$tmp/src" && "$(command -v python3 || command -v python)" -c \
            'import zipfile; z = zipfile.ZipFile("../rt.zip", "w"); z.write("lib/runtime.dll"); z.close()')
    fi
    # What an extraction killed part-way used to leave: a directory with some of
    # the files, at the final path, indistinguishable from a finished one.
    : > "$FETCHCONTENT_CACHE_ROOT/rt-v1/partial-file"

    # curl is a native program on Windows and cannot open an MSYS /tmp path.
    zip_url="file://$tmp/rt.zip"
    if command -v cygpath >/dev/null 2>&1; then
        zip_url="file:///$(cygpath -m "$tmp/rt.zip")"
    fi
    rc=0
    ensure_shared_archive_source "Runtime" "$zip_url" "rt-v1" "$tmp/no-seed" \
        > "$tmp/out" 2>&1 || rc=$?
    check "$rc" "0" "priming succeeds"
    check "$(grep -c 'is incomplete; re-fetching it' "$tmp/out")" "1" \
        "the unmarked directory is not trusted"
    check "$(cat "$FETCHCONTENT_CACHE_ROOT/rt-v1/lib/runtime.dll" 2>/dev/null)" "runtime" \
        "the archive is extracted"
    check "$([ -e "$FETCHCONTENT_CACHE_ROOT/rt-v1/partial-file" ] && echo yes || echo no)" "no" \
        "nothing from the partial copy survives"
    check "$([ -e "$FETCHCONTENT_CACHE_ROOT/.rt-v1.complete" ] && echo yes || echo no)" "yes" \
        "the completion marker is written"

    # A download that fails must leave no marker, even when the caller tests the
    # result (which disables the priming subshell's set -e).
    rc=0
    ensure_shared_archive_source "Broken" "file://$tmp/missing.zip" "broken-v1" "$tmp/no-seed" \
        > "$tmp/out-broken" 2>&1 || rc=$?
    check "$([ "$rc" != 0 ] && echo failed || echo succeeded)" "failed" "a failed download fails priming"
    check "$([ -e "$FETCHCONTENT_CACHE_ROOT/.broken-v1.complete" ] && echo yes || echo no)" "no" \
        "a failed download writes no completion marker"
    check "$([ -e "$FETCHCONTENT_CACHE_ROOT/broken-v1" ] && echo yes || echo no)" "no" \
        "a failed download leaves no cache directory"

    : > "$tmp/out"
    ensure_shared_archive_source "Runtime" "file://$tmp/missing.zip" "rt-v1" "$tmp/no-seed" \
        > "$tmp/out" 2>&1
    check "$?" "0" "a marked cache is reused without downloading again"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo
if [ "$FAIL" -gt 0 ]; then
    echo "FAILED ($FAIL failing group(s))"
    exit 1
fi
echo "All setup.sh cache lock tests passed."
