#!/usr/bin/env bash
# test_setup_source_cache.sh — unit tests for setup.sh's shared source cache.
#
# Covers the case where a dependency is re-pinned to a new version while a cache
# for the OLD version is already on the machine. find_local_git_seed matches on
# origin URL alone, so the old cache seeds the new one; the caches are
# `--filter=blob:none` partial clones, and a local clone of one copies the
# incomplete object store but not the promisor configuration. Everything here
# runs against throwaway file:// repos — no network, no real cache.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SETUP_SH="$REPO_ROOT/setup.sh"

# Ignore the developer's git config: a global tag.forceSignAnnotated or
# commit.gpgsign would break fixture creation, and a fixture that fails to
# build silently is worse than one that fails loudly (a missing tag makes the
# seed clone land on the default branch, which already holds v2's blobs — the
# tests then pass without ever exercising the bug).
export GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_SYSTEM=/dev/null

PASS=0
FAIL=0

ok()   { echo "  ✓ $*"; PASS=$((PASS + 1)); }
bad()  { echo "  ✗ $*"; FAIL=$((FAIL + 1)); }
check() { if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (want '$2', got '$1')"; fi; }

# Sourced with no positional args: setup.sh parses "$@" at file scope.
load_setup_lib() {
    set --
    # shellcheck disable=SC1090
    PULP_SETUP_LIB_ONLY=1 . "$SETUP_SH"
    set +e  # setup.sh sets -e; the harness runs assertions past failures.
}

# An upstream whose LICENSE.txt CHANGES between the two tags, so v2's blob can
# never be present in a v1 clone — the exact shape of the VST3 3.7.12 -> 3.8.0
# re-pin, where 3.8.0 relicensed LICENSE.txt and added README.md.
make_upstream() {
    local dir="$1"
    git init -q "$dir"
    git -C "$dir" config user.email t@t.t
    git -C "$dir" config user.name t
    # Partial clone over file:// requires the server side to allow filtering.
    git -C "$dir" config uploadpack.allowFilter true
    git -C "$dir" config uploadpack.allowAnySHA1InWant true

    printf 'OLD LICENSE\n' > "$dir/LICENSE.txt"
    git -C "$dir" add LICENSE.txt
    git -C "$dir" commit -qm v1
    git -C "$dir" tag v1

    printf 'NEW LICENSE\n' > "$dir/LICENSE.txt"
    printf 'readme\n' > "$dir/README.md"
    git -C "$dir" add LICENSE.txt README.md
    git -C "$dir" commit -qm v2
    git -C "$dir" tag v2
}

# The v1 cache as setup.sh would have left it: a blob:none partial clone that
# has only ever checked out v1, so v2's differing blobs were never fetched.
# --no-checkout matters: a plain clone would check out the default branch (v2)
# and lazily fetch exactly the blobs whose absence these tests depend on.
make_v1_cache() {
    local upstream="$1" cache="$2"
    git clone -q --filter=blob:none --no-checkout "file://$upstream" "$cache/dep-v1" 2>/dev/null || {
        echo "FIXTURE ERROR: could not partial-clone upstream"; exit 1; }
    git -C "$cache/dep-v1" checkout -q --detach v1 || {
        echo "FIXTURE ERROR: could not check out v1"; exit 1; }
    # The seed must NOT already hold v2's LICENSE.txt blob, or the tests below
    # would pass without ever exercising the missing-blob path.
    if [ "$(cat "$cache/dep-v1/LICENSE.txt")" != "OLD LICENSE" ]; then
        echo "FIXTURE ERROR: seed is not at v1"; exit 1
    fi
}

echo "== ensure_shared_git_source: re-pin with an old-version cache present"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    make_upstream "$tmp/upstream"
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    mkdir -p "$FETCHCONTENT_CACHE_ROOT"
    make_v1_cache "$tmp/upstream" "$FETCHCONTENT_CACHE_ROOT"

    # Precondition: the seed really is a partial clone missing v2's blobs.
    check "$(git -C "$FETCHCONTENT_CACHE_ROOT/dep-v1" config remote.origin.promisor)" \
        "true" "v1 cache is a partial clone (test precondition)"

    rc=0
    out="$(ensure_shared_git_source "Dep" "file://$tmp/upstream" "v2" "dep-v2" 2>&1)" || rc=$?
    new="$FETCHCONTENT_CACHE_ROOT/dep-v2"

    check "$rc" "0" "priming v2 from the v1 cache succeeds"
    # The seed's promisor wiring is carried over up front, so the first
    # checkout materializes v2's blobs directly and the repair path never runs.
    check "$(printf '%s' "$out" | grep -c 'missing objects')" "0" \
        "a fresh seed needs no repair pass"
    check "$(printf '%s' "$out" | grep -c 'unable to read sha1')" "0" \
        "a fresh seed reports no unreadable objects"
    check "$([ -f "$new/LICENSE.txt" ] && echo yes || echo no)" "yes" \
        "v2 LICENSE.txt is materialized, not left deleted"
    check "$(cat "$new/LICENSE.txt" 2>/dev/null)" "NEW LICENSE" \
        "v2 LICENSE.txt has v2's content, not the seed's"
    check "$([ -f "$new/README.md" ] && echo yes || echo no)" "yes" \
        "a file that exists only in v2 is materialized"
    check "$(git -C "$new" status --porcelain 2>/dev/null | wc -l | tr -d ' ')" "0" \
        "v2 worktree is complete (nothing left deleted)"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== ensure_shared_git_source: self-heals a cache poisoned by an earlier run"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    make_upstream "$tmp/upstream"
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    mkdir -p "$FETCHCONTENT_CACHE_ROOT"
    make_v1_cache "$tmp/upstream" "$FETCHCONTENT_CACHE_ROOT"

    # Reproduce the damage a pre-fix run left behind: object store copied from
    # the v1 partial clone, promisor wiring dropped, files missing.
    # `git clone --local` copies the seed's blob-less object store and drops its
    # promisor config, so v2's commit is present but its blobs are neither
    # available nor fetchable. Checking out v2 then deletes those paths.
    poisoned="$FETCHCONTENT_CACHE_ROOT/dep-v2"
    git clone -q --local --no-hardlinks "$FETCHCONTENT_CACHE_ROOT/dep-v1" "$poisoned" 2>/dev/null
    git -C "$poisoned" remote set-url origin "file://$tmp/upstream"
    git -C "$poisoned" checkout -q --detach v2 2>/dev/null
    check "$(git -C "$poisoned" status --porcelain | wc -l | tr -d ' ')" "2" \
        "poisoned cache starts with deleted files (test precondition)"

    rc=0
    ensure_shared_git_source "Dep" "file://$tmp/upstream" "v2" "dep-v2" >/dev/null 2>&1 || rc=$?
    check "$rc" "0" "re-priming repairs rather than failing"
    check "$(cat "$poisoned/LICENSE.txt" 2>/dev/null)" "NEW LICENSE" \
        "poisoned cache's missing blobs are restored"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== git_worktree_is_complete"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    git init -q "$tmp/r"
    git -C "$tmp/r" config user.email t@t.t
    git -C "$tmp/r" config user.name t
    printf 'x\n' > "$tmp/r/f.txt"
    git -C "$tmp/r" add f.txt
    git -C "$tmp/r" commit -qm c

    git_worktree_is_complete "$tmp/r" && r=yes || r=no
    check "$r" "yes" "a clean checkout reads as complete"

    rm "$tmp/r/f.txt"
    git_worktree_is_complete "$tmp/r" && r=yes || r=no
    check "$r" "no" "a checkout with a deleted file reads as incomplete"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== git_worktree_is_complete: submodules pending update are not incompleteness"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

    git init -q "$tmp/sub"
    git -C "$tmp/sub" config user.email t@t.t
    git -C "$tmp/sub" config user.name t
    printf 'one\n' > "$tmp/sub/s.txt"
    git -C "$tmp/sub" add s.txt; git -C "$tmp/sub" commit -qm s1
    printf 'two\n' > "$tmp/sub/s.txt"
    git -C "$tmp/sub" add s.txt; git -C "$tmp/sub" commit -qm s2
    sub2="$(git -C "$tmp/sub" rev-parse HEAD)"
    sub1="$(git -C "$tmp/sub" rev-parse HEAD~1)"

    # A parent whose submodule pointer MOVES between the two tags — the shape
    # of the VST3 SDK, which carries five sub-submodules across a re-pin.
    git init -q "$tmp/par"
    git -C "$tmp/par" config user.email t@t.t
    git -C "$tmp/par" config user.name t
    git -C "$tmp/par" -c protocol.file.allow=always submodule add -q "$tmp/sub" sub 2>/dev/null
    git -C "$tmp/par/sub" checkout -q "$sub1"
    git -C "$tmp/par" add .; git -C "$tmp/par" commit -qm v1; git -C "$tmp/par" tag v1
    git -C "$tmp/par/sub" checkout -q "$sub2"
    git -C "$tmp/par" add .; git -C "$tmp/par" commit -qm v2; git -C "$tmp/par" tag v2

    # Re-pin v1 -> v2 with the submodule not yet updated: `submodule update`
    # runs after the checkout, so this state is normal, not damage.
    git -C "$tmp/par" checkout -q --detach v1
    git -C "$tmp/par" -c protocol.file.allow=always submodule update -q 2>/dev/null
    git -C "$tmp/par" checkout -q --detach v2

    check "$(git -C "$tmp/par" status --porcelain | wc -l | tr -d ' ')" "1" \
        "a pending submodule does show as modified (test precondition)"
    git_worktree_is_complete "$tmp/par" && r=yes || r=no
    check "$r" "yes" "a pending submodule is not mistaken for a missing blob"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== reuse_shared_git_source: a re-pin re-points a stale symlink"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    PLATFORM=macOS   # the symlink branch; Windows clones instead

    # Two shared caches, as a re-pin leaves them: the directory name embeds the
    # ref, so the OLD one survives next to the new one.
    for ref in v1 v2; do
        mkdir -p "$tmp/cache/dep-$ref/marker"
        git init -q "$tmp/cache/dep-$ref"
        git -C "$tmp/cache/dep-$ref" config user.email t@t.t
        git -C "$tmp/cache/dep-$ref" config user.name t
        git -C "$tmp/cache/dep-$ref" add marker 2>/dev/null || true
        printf '%s\n' "$ref" > "$tmp/cache/dep-$ref/marker/f"
        git -C "$tmp/cache/dep-$ref" add -A
        git -C "$tmp/cache/dep-$ref" commit -qm "$ref"
    done

    mkdir -p "$tmp/repo/external"
    ln -s "$tmp/cache/dep-v1" "$tmp/repo/external/dep"   # what the v1 setup left

    rc=0
    reuse_shared_git_source "Dep" "$tmp/cache/dep-v2" "$tmp/repo/external/dep" "marker" >/dev/null 2>&1 || rc=$?

    check "$rc" "0" "re-pinning an existing link succeeds"
    check "$(readlink "$tmp/repo/external/dep")" "$tmp/cache/dep-v2" \
        "the link now points at the NEW pin's cache"
    check "$(cat "$tmp/repo/external/dep/marker/f" 2>/dev/null)" "v2" \
        "the tree reachable through it is the new version"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== reuse_shared_git_source: an already-correct link is left alone"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    PLATFORM=macOS

    mkdir -p "$tmp/cache/dep-v2/marker" "$tmp/repo/external"
    git init -q "$tmp/cache/dep-v2"
    ln -s "$tmp/cache/dep-v2" "$tmp/repo/external/dep"
    before="$(readlink "$tmp/repo/external/dep")"

    rc=0
    out="$(reuse_shared_git_source "Dep" "$tmp/cache/dep-v2" "$tmp/repo/external/dep" "marker" 2>&1)" || rc=$?

    check "$rc" "0" "a current link succeeds"
    check "$(readlink "$tmp/repo/external/dep")" "$before" "a current link is not touched"
    check "$(printf '%s' "$out" | grep -c 'stale')" "0" "a current link is not called stale"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== reuse_shared_git_source: a drifted REAL directory is reported, never deleted"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    PLATFORM=macOS

    # Shared cache at the pinned ref, and a hand-managed checkout a commit behind.
    git init -q "$tmp/cache/dep-v2"
    git -C "$tmp/cache/dep-v2" config user.email t@t.t
    git -C "$tmp/cache/dep-v2" config user.name t
    mkdir -p "$tmp/cache/dep-v2/marker"
    printf 'v2\n' > "$tmp/cache/dep-v2/marker/f"
    git -C "$tmp/cache/dep-v2" add -A; git -C "$tmp/cache/dep-v2" commit -qm v2

    mkdir -p "$tmp/repo/external"
    git clone -q "$tmp/cache/dep-v2" "$tmp/repo/external/dep"
    printf 'v1\n' > "$tmp/repo/external/dep/marker/f"
    git -C "$tmp/repo/external/dep" -c user.email=t@t.t -c user.name=t commit -qam drift

    ERRORS=0
    rc=0
    out="$(reuse_shared_git_source "Dep" "$tmp/cache/dep-v2" "$tmp/repo/external/dep" "marker" 2>&1)" || rc=$?

    check "$rc" "1" "a drifted real checkout fails rather than being used"
    check "$(printf '%s' "$out" | grep -c 'not at the pinned ref')" "1" \
        "the failure names the drift"
    check "$([ -d "$tmp/repo/external/dep" ] && echo yes || echo no)" "yes" \
        "a developer-managed directory is never deleted"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== reuse_shared_git_source: a real directory AT the pin is accepted"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    PLATFORM=macOS

    git init -q "$tmp/cache/dep-v2"
    git -C "$tmp/cache/dep-v2" config user.email t@t.t
    git -C "$tmp/cache/dep-v2" config user.name t
    mkdir -p "$tmp/cache/dep-v2/marker"
    printf 'v2\n' > "$tmp/cache/dep-v2/marker/f"
    git -C "$tmp/cache/dep-v2" add -A; git -C "$tmp/cache/dep-v2" commit -qm v2

    mkdir -p "$tmp/repo/external"
    git clone -q "$tmp/cache/dep-v2" "$tmp/repo/external/dep"

    ERRORS=0
    rc=0
    reuse_shared_git_source "Dep" "$tmp/cache/dep-v2" "$tmp/repo/external/dep" "marker" >/dev/null 2>&1 || rc=$?
    check "$rc" "0" "a real checkout on the pinned commit is accepted"
    check "$ERRORS" "0" "and is not reported as an error"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== git_checkout_matches_cache: an unknowable tree is never called drifted"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

    # An unpacked release with no .git — the setup cannot tell, so it must not
    # guess "drifted" and delete a developer's tree out from under them.
    mkdir -p "$tmp/plain" "$tmp/cache"
    git_checkout_matches_cache "$tmp/plain" "$tmp/cache/dep" && r=yes || r=no
    check "$r" "yes" "a non-git tree is treated as matching"

    git init -q "$tmp/real"
    git_checkout_matches_cache "$tmp/real" "$tmp/cache/missing" && r=yes || r=no
    check "$r" "yes" "an unprovisioned cache is treated as matching"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo
if [ "$FAIL" -gt 0 ]; then
    echo "FAILED ($FAIL failing group(s))"
    exit 1
fi
echo "All setup.sh source-cache tests passed."
