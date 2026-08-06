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

    git -C "$tmp/r" checkout -q -- f.txt
    printf 'developer modification\n' > "$tmp/r/f.txt"
    printf 'untracked\n' > "$tmp/r/other.txt"
    git_worktree_is_complete "$tmp/r" && r=yes || r=no
    check "$r" "yes" "modified or untracked files do not authorize cache deletion"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== retry discards a cache whose commit exists but worktree is incomplete"
(
    load_setup_lib
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    export FETCHCONTENT_CACHE_ROOT="$tmp/cache"
    mkdir -p "$FETCHCONTENT_CACHE_ROOT/dep-v2"
    git init -q "$FETCHCONTENT_CACHE_ROOT/dep-v2"
    git -C "$FETCHCONTENT_CACHE_ROOT/dep-v2" config user.email t@t.t
    git -C "$FETCHCONTENT_CACHE_ROOT/dep-v2" config user.name t
    printf 'complete\n' > "$FETCHCONTENT_CACHE_ROOT/dep-v2/tracked.txt"
    git -C "$FETCHCONTENT_CACHE_ROOT/dep-v2" add tracked.txt
    git -C "$FETCHCONTENT_CACHE_ROOT/dep-v2" commit -qm fixture
    ref="$(git -C "$FETCHCONTENT_CACHE_ROOT/dep-v2" rev-parse HEAD)"
    rm "$FETCHCONTENT_CACHE_ROOT/dep-v2/tracked.txt"

    calls=0
    ensure_shared_git_source() {
        calls=$((calls + 1))
        if [ "$calls" -eq 1 ]; then
            return 1
        fi
        [ ! -e "$FETCHCONTENT_CACHE_ROOT/dep-v2" ] || return 2
        mkdir -p "$FETCHCONTENT_CACHE_ROOT/dep-v2"
        return 0
    }
    sleep() { :; }
    PULP_PRIMING_RETRY_ATTEMPTS=2
    rc=0
    ensure_shared_git_source_with_retry "Dep" fixture "$ref" dep-v2 >/dev/null 2>&1 || rc=$?
    check "$rc" "0" "an incomplete worktree is scrubbed before the retry"
    check "$calls" "2" "the clean retry runs exactly once"

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

echo "== the three.js seed and the CMake registration agree on a cache directory"
(
    load_setup_lib
    repo_root="$(cd "$(dirname "$0")/../.." && pwd)"

    # three.js is the largest FetchContent source. Its ref lives in setup.sh (to
    # seed the shared cache) and in PulpDependencies.cmake (to register the
    # override). If those drift, nothing fails loudly — the cache is populated
    # under one directory name, CMake looks for another, and every build dir
    # silently re-clones ~2.2 GB forever. Pin the agreement.
    cmake_ref="$(sed -n 's/.*pulp_register_fetchcontent_source(threejs REF \([0-9a-f]*\)).*/\1/p' \
        "$repo_root/tools/cmake/PulpDependencies.cmake" | head -1)"
    setup_ref="$(grep -A2 'mrdoob/three\.js\.git' "$repo_root/setup.sh" \
        | grep -oE '[0-9a-f]{40}' | head -1)"

    check "$(test -n "$cmake_ref" && echo found || echo missing)" "found" \
        "PulpDependencies.cmake registers a threejs ref"
    check "$(test -n "$setup_ref" && echo found || echo missing)" "found" \
        "setup.sh seeds a three.js source"
    check "$setup_ref" "$cmake_ref" "setup.sh and PulpDependencies.cmake pin the same three.js ref"
    contract_ref="$(tr ';' '\n' < "$repo_root/tools/deps/shared-source-contract.txt" \
        | sed -n 's/^threejs=//p' | head -1)"
    check "$contract_ref" "$cmake_ref" "dependency contract records three.js pin"

    # And the directory name setup.sh writes must be the one CMake reads.
    check "$(fetchcontent_cache_dir_name "threejs" "$setup_ref")" "threejs-$cmake_ref" \
        "the seeded cache directory is the one the CMake override resolves to"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== all shared FetchContent registrations seeded by setup agree with CMake"
(
    load_setup_lib
    repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
    deps="$repo_root/tools/cmake/PulpDependencies.cmake"
    root_cmake="$repo_root/CMakeLists.txt"
    contract="$repo_root/tools/deps/shared-source-contract.txt"

    registered_ref() {
        local name="$1" file="$2"
        sed -n "s/.*pulp_register_fetchcontent_source(${name}\( DIR [^ ]*\)\{0,1\} REF \([^)]*\)).*/\2/p" "$file" | head -1
    }
    seeded_ref() {
        local url_pattern="$1"
        grep -A1 "$url_pattern" "$repo_root/setup.sh" \
            | tail -1 | sed -n 's/.*"\([^"]*\)" "$(fetchcontent.*/\1/p'
    }
    contract_ref() {
        local name="$1"
        tr ';' '\n' < "$contract" | sed -n "s/^${name}=//p" | head -1
    }

    seeded_names="$(grep -oE 'fetchcontent_cache_dir_name "[^"]+"' "$repo_root/setup.sh" \
        | cut -d'"' -f2 | grep -v '^\$' | sort -fu | tr '\n' ' ' | sed 's/ $//')"
    check "$seeded_names" "AudioUnitSDK catch2 choc clap lv2 sdl3 threejs vst3sdk webgpu yoga" \
        "pin guard enumerates every statically named setup source seed"
    contract_names="$(tr ';' '\n' < "$contract" | sed -n 's/^\([^=]*\)=.*/\1/p' \
        | sort -fu | tr '\n' ' ' | sed 's/ $//')"
    check "$contract_names" "ausdk catch2 choc clap lv2 sdl3 threejs vst3 webgpu wgpu-native yoga" \
        "dependency contract enumerates every provisioned source and runtime"

    # These two previously drifted while the three.js-only contract remained
    # green. Setup then populated a cache directory CMake never consulted, so
    # every build directory could silently clone another dependency copy.
    choc_cmake_ref="$(sed -n 's/.*pulp_register_fetchcontent_source(choc REF \([^)]*\)).*/\1/p' "$deps" | head -1)"
    choc_setup_cache_ref="$(grep -A1 'danielraffel/choc\.git' "$repo_root/setup.sh" \
        | grep -oE '[0-9a-f]{40}' | head -1)"
    clap_cmake_ref="$(registered_ref clap "$deps")"
    clap_setup_ref="$(seeded_ref 'free-audio/clap\.git')"

    check "$(test -n "$choc_cmake_ref" && echo found || echo missing)" "found" \
        "PulpDependencies.cmake registers a CHOC ref"
    check "$(test -n "$choc_setup_cache_ref" && echo found || echo missing)" "found" \
        "setup.sh names a CHOC cache ref"
    check "$choc_setup_cache_ref" "$choc_cmake_ref" \
        "setup.sh and PulpDependencies.cmake pin the same CHOC cache"
    check "$(contract_ref choc)" "$choc_cmake_ref" "dependency contract records CHOC pin"
    check "$(test -n "$clap_cmake_ref" && echo found || echo missing)" "found" \
        "PulpDependencies.cmake registers a CLAP ref"
    check "$(test -n "$clap_setup_ref" && echo found || echo missing)" "found" \
        "setup.sh seeds a CLAP source"
    check "$clap_setup_ref" "$clap_cmake_ref" \
        "setup.sh and PulpDependencies.cmake pin the same CLAP source"
    check "$(contract_ref clap)" "$clap_cmake_ref" "dependency contract records CLAP pin"

    for spec in \
        'webgpu|eliemichel/WebGPU-distribution\.git|webgpu|tools' \
        'SDL3|libsdl-org/SDL\.git|SDL3|tools' \
        'lv2|github.com/lv2/lv2\.git|lv2|tools' \
        'yoga|facebook/yoga\.git|yoga|tools' \
        'Catch2|catchorg/Catch2\.git|Catch2|root'; do
        IFS='|' read -r display url name cmake_file <<< "$spec"
        if [ "$cmake_file" = "root" ]; then
            expected="$(registered_ref "$name" "$root_cmake")"
        else
            expected="$(registered_ref "$name" "$deps")"
        fi
        actual="$(seeded_ref "$url")"
        check "$(test -n "$expected" && test -n "$actual" && echo found || echo missing)" "found" \
            "$display has both a setup seed and CMake registration"
        check "$actual" "$expected" "$display setup and CMake pins agree"
        check "$(contract_ref "$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')")" "$expected" \
            "$display pin is recorded in the dependency contract"
    done

    check "$(contract_ref wgpu-native)" "$(grep '^WGPU_NATIVE_VERSION=' "$repo_root/setup.sh" | head -1 | cut -d'"' -f2)" \
        "dependency contract records wgpu-native runtime pin"
    check "$(contract_ref vst3)" "$(grep '^VST3_SDK_REF=' "$repo_root/setup.sh" | head -1 | cut -d'"' -f2)" \
        "dependency contract records VST3 pin"
    check "$(contract_ref ausdk)" "$(grep '^    AU_SDK_REF=' "$repo_root/setup.sh" | head -1 | cut -d'"' -f2)" \
        "dependency contract records AudioUnitSDK pin"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== non-interactive setup never accepts an install prompt"
(
    load_setup_lib
    NON_INTERACTIVE=true
    CI_MODE=false
    prompt_yn "install fixture" && result=yes || result=no
    check "$result" "no" "--non-interactive declines package installation"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== dependency-only status ignores unrelated earlier audit findings"
(
    load_setup_lib
    CI_MODE=false
    ERRORS=4
    DEPENDENCY_ERRORS_START=$ERRORS
    check "$(dependency_bootstrap_error_count)" "0" \
        "earlier environment findings do not fail dependency bootstrap"
    ERRORS=$((ERRORS + 1))
    check "$(dependency_bootstrap_error_count)" "1" \
        "a provisioning failure still fails dependency bootstrap"
    check "$(dependency_bootstrap_exit_error_count)" "1" \
        "local dependency-only exit ignores earlier host audit findings"
    CI_MODE=true
    check "$(dependency_bootstrap_exit_error_count)" "5" \
        "CI dependency-only host admission preserves every audit failure"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== shared cache location is machine-configurable"
(
    load_setup_lib
    PLATFORM=macOS
    PULP_SHARED_FETCHCONTENT_SOURCE_DIR="/example/mounted-volume/pulp-cache"
    check "$(fetchcontent_cache_root)" "$PULP_SHARED_FETCHCONTENT_SOURCE_DIR" \
        "explicit cache root wins without machine-name or checkout-path assumptions"
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== explicit source override below the cache root remains developer-managed"
(
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/cache/choc-dev" "$tmp/src"
    printf 'developer edit\n' > "$tmp/cache/choc-dev/value.txt"
    cat > "$tmp/src/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.24)
project(explicit_override NONE)
set(PULP_SHARED_FETCHCONTENT_SOURCE_DIR "$tmp/cache")
set(PULP_USE_SHARED_FETCHCONTENT_SOURCES ON)
set(FETCHCONTENT_SOURCE_DIR_CHOC "$tmp/cache/choc-dev" CACHE PATH "")
include("$REPO_ROOT/tools/cmake/PulpFetchContent.cmake")
pulp_register_fetchcontent_source(choc REF fixture)
pulp_materialize_mutable_fetchcontent_source(choc PATCH_KEY fixture)
file(WRITE "\${CMAKE_BINARY_DIR}/selected.txt" "\${FETCHCONTENT_SOURCE_DIR_CHOC}")
EOF
    cmake -S "$tmp/src" -B "$tmp/build" >/dev/null 2>&1
    check "$(cat "$tmp/build/selected.txt")" "$tmp/cache/choc-dev" \
        "an explicit override is not replaced with a frozen build-local copy"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== a warm build follows shared cache pin changes without exposing the cache"
(
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/cache/choc-v1" "$tmp/cache/choc-v2" "$tmp/src"
    printf 'v1\n' > "$tmp/cache/choc-v1/value.txt"
    printf 'v2\n' > "$tmp/cache/choc-v2/value.txt"
    cat > "$tmp/src/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.24)
project(shared_pin_change NONE)
set(PULP_SHARED_FETCHCONTENT_SOURCE_DIR "$tmp/cache")
set(PULP_USE_SHARED_FETCHCONTENT_SOURCES ON)
include("$REPO_ROOT/tools/cmake/PulpFetchContent.cmake")
pulp_register_fetchcontent_source(choc REF "\${TEST_REF}")
pulp_materialize_mutable_fetchcontent_source(choc PATCH_KEY "\${TEST_REF}")
file(WRITE "\${CMAKE_BINARY_DIR}/selected.txt" "\${FETCHCONTENT_SOURCE_DIR_CHOC}")
EOF
    cmake -S "$tmp/src" -B "$tmp/build" -DTEST_REF=v1 >/dev/null 2>&1
    cmake -S "$tmp/src" -B "$tmp/build" -DTEST_REF=v2 >/dev/null 2>&1
    selected="$(cat "$tmp/build/selected.txt")"
    check "$selected" "$tmp/build/_deps/pulp-mutable-choc-src" \
        "a pin bump keeps the patched dependency build-local"
    check "$(cat "$selected/value.txt")" "v2" \
        "the build-local copy refreshes from the newly pinned cache"
    check "$(grep '^FETCHCONTENT_SOURCE_DIR_CHOC:PATH=' "$tmp/build/CMakeCache.txt" | cut -d= -f2-)" \
        "$tmp/cache/choc-v2" "the automatic cache selection advances to the new pin"

    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo "== shared dependency links are location-independent"
(
    load_setup_lib
    PLATFORM=macOS
    temp_root="${TMPDIR:-/tmp}"
    checkout_root="${PULP_TEST_SECOND_VOLUME_ROOT:-$temp_root}"
    shared="$(mktemp -d "$temp_root/pulp-shared-cache-fixture.XXXXXX")"
    checkout="$(mktemp -d "$checkout_root/pulp-worktree-fixture.XXXXXX")"
    trap 'rm -rf "$shared" "$checkout"' EXIT
    mkdir -p "$shared/marker" "$checkout/external"
    printf 'shared\n' > "$shared/marker/value"
    git init -q "$shared"
    git -C "$shared" config user.email fixture@pulp.audio
    git -C "$shared" config user.name fixture
    git -C "$shared" add marker/value
    git -C "$shared" commit -qm fixture

    ERRORS=0
    reuse_shared_git_source "location-independent fixture" "$shared" \
        "$checkout/external/dependency" "marker/value" >/dev/null
    check "$(cat "$checkout/external/dependency/marker/value")" "shared" \
        "worktree link resolves a cache outside the checkout"
    check "$ERRORS" "0" "location-independent link reports no setup error"
    if [ -n "${PULP_TEST_SECOND_VOLUME_ROOT:-}" ]; then
        if stat -f '%d' "$shared" >/dev/null 2>&1; then
            shared_device="$(stat -f '%d' "$shared")"
            checkout_device="$(stat -f '%d' "$checkout")"
        else
            shared_device="$(stat -c '%d' "$shared")"
            checkout_device="$(stat -c '%d' "$checkout")"
        fi
        if [ "$shared_device" != "$checkout_device" ]; then
            ok "explicit second-volume fixture really crosses filesystems"
        else
            bad "PULP_TEST_SECOND_VOLUME_ROOT must identify a different filesystem"
        fi
    fi
    exit $((FAIL > 0))
) || FAIL=$((FAIL + 1))

echo
if [ "$FAIL" -gt 0 ]; then
    echo "FAILED ($FAIL failing group(s))"
    exit 1
fi
echo "All setup.sh source-cache tests passed."
