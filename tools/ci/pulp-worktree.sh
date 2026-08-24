#!/usr/bin/env bash
# pulp-worktree.sh — per-branch git worktrees with isolated build dirs and a
# SHARED build cache, so parallel branch work never churns one build/ dir
# (which caused ODR heap corruption: a hung free() in View::~View when objects
# compiled against different IRStyle layouts were mixed).
#
# Each worktree gets its OWN build/, but ccache + Skia + FetchContent sources
# are shared from one cache root. ccache is configured to actually share across
# worktrees: CCACHE_BASEDIR points at the common worktree PARENT, not a single
# workspace, so absolute paths normalize and hit-rates hold.
#
# Build dirs are disposable; ccache is the durable artifact. `gc` reclaims disk.
#
# Usage:
#   pulp-worktree.sh new <branch> [--base origin/main]   # create + configure
#   pulp-worktree.sh env <branch>                          # print cache env to source
#   pulp-worktree.sh list                                  # worktrees + build-dir sizes
#   pulp-worktree.sh gc [--max-total-gb N] [--max-age-days N] [--merged]
#
# Env overrides: PULP_WT_ROOT (default ../pulp-worktrees), PULP_CI_CACHE
# (default ~/.cache/pulp-ci), PULP_CCACHE_MAX_SIZE (default 200G).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WT_ROOT="${PULP_WT_ROOT:-$(cd "$REPO_ROOT/.." && pwd)/pulp-worktrees}"
CACHE_ROOT="${PULP_CI_CACHE:-$HOME/.cache/pulp-ci}"
if [ -n "${PULP_SHARED_FETCHCONTENT_SOURCE_DIR:-}" ]; then
  FETCHCONTENT_SOURCE_ROOT="$PULP_SHARED_FETCHCONTENT_SOURCE_DIR"
elif [ "$(uname -s)" = Darwin ]; then
  FETCHCONTENT_SOURCE_ROOT="$HOME/Library/Caches/Pulp/fetchcontent-src"
else
  FETCHCONTENT_SOURCE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/pulp/fetchcontent-src"
fi
SKIA_CACHE_KEYING="${PULP_SKIA_CACHE_KEYING:-1}"
SKIA_CACHE_ROOT="${PULP_SKIA_CACHE_ROOT:-$HOME/.cache/pulp/skia}"
CCACHE_MAX_SIZE="${PULP_CCACHE_MAX_SIZE:-200G}"

# CCACHE_BASEDIR is the COMMON PARENT of the repo and every worktree so a hit
# in /a/pulp-worktrees/X normalizes against /a/pulp-worktrees/Y and the primary
# checkout. WT_ROOT defaults to <repo-parent>/pulp-worktrees, so its parent is
# the shared root. Computed with dirname (WT_ROOT may not exist yet).
BASEDIR="$(dirname "$WT_ROOT")"

note() { printf '\033[36m• %s\033[0m\n' "$*" >&2; }
die()  { printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }

resolve_skia_cache_dir() {
  local repo="$1"
  if [ "$SKIA_CACHE_KEYING" = 1 ] && [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ]; then
    # Use this helper revision's cache-aware fetcher while reading the target
    # worktree's manifest from cwd. Older branches need not implement the new
    # cache CLI in order to be opened or resumed safely.
    (cd "$repo" && python3 "$REPO_ROOT/tools/scripts/fetch_skia_for_release.py" \
      darwin-arm64 --cache-root "$SKIA_CACHE_ROOT" --print-cache-dest) || \
      die "failed to resolve immutable Skia cache generation for $repo"
  elif [ "$SKIA_CACHE_KEYING" = 1 ]; then
    printf '%s\n' "$SKIA_CACHE_ROOT"
  else
    printf '%s\n' "${PULP_SKIA_CACHE:-$HOME/.cache/pulp/skia-build}"
  fi
}

ensure_cache() {
  local repo="${1:-$REPO_ROOT}"
  local skia_cache_dir
  skia_cache_dir="$(resolve_skia_cache_dir "$repo")"
  mkdir -p "$CACHE_ROOT"/{ccache,tmp} "$FETCHCONTENT_SOURCE_ROOT"
  if [ "$SKIA_CACHE_KEYING" = 1 ]; then
    mkdir -p "$SKIA_CACHE_ROOT"
  else
    mkdir -p "$skia_cache_dir"
  fi
  # Existence alone proves neither a materialized LFS asset nor its manifest
  # pin, architecture, or complete Skia/Dawn pair. Use the canonical validator
  # and immutable generation publisher instead of copying an arbitrary checkout.
  if [ "${PULP_WORKTREE_SKIP_SKIA_FETCH:-0}" != 1 ] && \
     [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ]; then
    note "validating shared pinned Skia cache"
    local skia_fetch_args=(--dest "$skia_cache_dir")
    if [ "$SKIA_CACHE_KEYING" = 1 ]; then
      skia_fetch_args=(--cache-root "$SKIA_CACHE_ROOT")
    fi
    (cd "$repo" && python3 "$REPO_ROOT/tools/scripts/fetch_skia_for_release.py" \
      darwin-arm64 "${skia_fetch_args[@]}" \
      --cache-lock-timeout "${PULP_SKIA_CACHE_LOCK_TIMEOUT_SECS:-300}") || \
      die "shared Skia cache validation/provisioning failed"
  fi
  command -v ccache >/dev/null 2>&1 && \
    CCACHE_DIR="$CACHE_ROOT/ccache" ccache --set-config "max_size=$CCACHE_MAX_SIZE" 2>/dev/null || true
}

cache_env() {  # emit the env every worktree build should use
  local repo="${1:-$REPO_ROOT}"
  local skia_cache_dir
  skia_cache_dir="$(resolve_skia_cache_dir "$repo")"
  cat <<ENV
export CCACHE_DIR="$CACHE_ROOT/ccache"
export CCACHE_BASEDIR="$BASEDIR"
export CCACHE_NOHASHDIR=true
# Depend mode OFF + content compiler keying — the #3504 correctness combo, the
# same one build.yml forces at job level. Depend mode with default mtime
# compiler keying on a cache SHARED across worktrees serves a stale/false-hit
# object that corrupts unrelated TUs (a pure function returns "" and
# change-unrelated tests fail) — the exact scar this shipyard/worktree lane
# would otherwise reproduce. ccache rejects CCACHE_DEPEND=false, so the negated
# NO-form is the env spelling; direct mode stays on (fast + correct once depend
# mode is off and the compiler is content-keyed). Depend mode stays OFF
# fleet-wide.
export CCACHE_NODEPEND=true
export CCACHE_COMPILERCHECK=content
export CCACHE_SLOPPINESS=time_macros
export PULP_SHARED_FETCHCONTENT_SOURCE_DIR="$FETCHCONTENT_SOURCE_ROOT"
# Make object paths relative so ccache hits survive different worktree paths
# (pairs with CCACHE_NOHASHDIR; required for Debug/RelWithDebInfo).
export CMAKE_CXX_FLAGS="\${CMAKE_CXX_FLAGS:-} -fdebug-prefix-map=$BASEDIR=."
ENV
  if [ "$SKIA_CACHE_KEYING" = 1 ] && [ -f "$repo/tools/cmake/PulpSkiaCache.cmake" ]; then
    # Do not freeze this worktree to today's manifest generation. CMake resolves
    # the exact current pin beneath this root on every fresh configure, so a
    # later rebase or manifest edit cannot silently keep stale Skia/Dawn bits.
    printf 'export PULP_SKIA_CACHE_ROOT="%s"\n' "$SKIA_CACHE_ROOT"
  elif [ "$SKIA_CACHE_KEYING" = 1 ]; then
    # Older target branches cannot resolve an immutable generation from the
    # root alone. Point their legacy CMake directly at the exact generation
    # that ensure_cache() just validated from that branch's own manifest.
    printf 'export PULP_SKIA_CACHE="%s"\n' "$skia_cache_dir"
    printf 'export SKIA_DIR="%s"\n' "$skia_cache_dir"
  else
    printf 'export PULP_SKIA_CACHE="%s"\n' "$skia_cache_dir"
    printf 'export SKIA_DIR="%s"\n' "$skia_cache_dir"
  fi
}

cmd_new() {
  local branch="$1"; shift || true
  local base="origin/main"
  [ "${1:-}" = "--base" ] && { base="$2"; shift 2; }
  [ -n "$branch" ] || die "usage: new <branch> [--base <ref>]"
  local wt="$WT_ROOT/$branch"
  mkdir -p "$WT_ROOT"
  git -C "$REPO_ROOT" fetch origin --quiet || true
  # A failed download/validation leaves the newly registered worktree intact
  # for diagnosis. Retrying `new` resumes that exact incomplete worktree rather
  # than failing at `git worktree add` or deleting user-visible state.
  if [ -e "$wt/.git" ] &&
     [ "$(git -C "$wt" symbolic-ref --quiet HEAD 2>/dev/null || true)" = "refs/heads/$branch" ]; then
    note "resuming existing worktree: $wt"
  elif git -C "$REPO_ROOT" show-ref --verify --quiet "refs/heads/$branch"; then
    git -C "$REPO_ROOT" worktree add "$wt" "$branch"
  else
    git -C "$REPO_ROOT" worktree add -b "$branch" "$wt" "$base"
  fi
  # Resolve and provision from the NEW worktree's own manifest. Branches may
  # pin different Skia revisions; exporting the primary checkout's generation
  # would make explicit SKIA_DIR suppress CMake's correct per-branch resolver.
  ensure_cache "$wt"
  local skia_cache_dir
  skia_cache_dir="$(resolve_skia_cache_dir "$wt")"
  # external/skia-build is tracked, so ln -sfn creates a nested link FindSkia
  # never consumes. The emitted absolute cache root is authoritative.
  if [ "${PULP_WORKTREE_LEGACY_SKIA_SYMLINK:-0}" = 1 ]; then
    ln -sfn "$skia_cache_dir" "$wt/external/skia-build" 2>/dev/null || true
  fi
  touch "$wt/build/.metadata_never_index" 2>/dev/null || { mkdir -p "$wt/build" && touch "$wt/build/.metadata_never_index"; }
  cache_env "$wt" > "$wt/.pulp-ci-env"
  note "worktree ready: $wt"
  note "configure with:  cd '$wt' && source .pulp-ci-env && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
}

cmd_env()  {
  local branch="${1:-}"
  local repo="$REPO_ROOT"
  if [ -n "$branch" ] && [ -f "$WT_ROOT/$branch/tools/scripts/fetch_skia_for_release.py" ]; then
    repo="$WT_ROOT/$branch"
  fi
  cache_env "$repo"
}
cmd_list() {
  git -C "$REPO_ROOT" worktree list
  echo "--- build-dir sizes ---"
  for wt in "$WT_ROOT"/*/; do [ -d "$wt/build" ] && du -sh "$wt/build" 2>/dev/null; done
  echo "--- ccache ---"
  CCACHE_DIR="$CACHE_ROOT/ccache" ccache --show-stats 2>/dev/null | grep -iE "cache size|hit rate|hits|misses" || true
}
cmd_gc() {
  local max_age="" merged=0
  while [ $# -gt 0 ]; do case "$1" in
    --max-age-days) max_age="$2"; shift 2;;
    --merged) merged=1; shift;;
    *) die "unknown gc arg: $1";; esac; done
  # Auto-cleanup of merged branches. Pulp SQUASH-merges and shipyard
  # auto-merge runs --delete-branch, so a merged branch's original commits are
  # NOT ancestors of origin/main — the reliable signal is its upstream going
  # "[gone]" after a pruning fetch. This also avoids false-pruning a brand-new
  # branch that was never pushed (no upstream → not [gone]).
  if [ "$merged" = 1 ]; then
    git -C "$REPO_ROOT" fetch -p origin --quiet 2>/dev/null || true
    local gone; gone="$(git -C "$REPO_ROOT" for-each-ref \
      --format '%(refname:short) %(upstream:track)' refs/heads \
      | awk '$2=="[gone]"{print $1}')"
    local b wt
    for b in $gone; do
      wt="$(git -C "$REPO_ROOT" worktree list --porcelain \
        | awk -v br="refs/heads/$b" 'f&&$1=="branch"&&$2==br{print p} {if($1=="worktree")p=$2; f=1}')"
      [ -n "$wt" ] && [ "$wt" != "$REPO_ROOT" ] && {
        note "merged+deleted upstream → pruning worktree $wt ($b)"
        git -C "$REPO_ROOT" worktree remove --force "$wt" 2>/dev/null || true
      }
      git -C "$REPO_ROOT" branch -D "$b" 2>/dev/null || true
    done
  fi
  [ -n "$max_age" ] && find "$WT_ROOT" -maxdepth 2 -name build -type d -mtime +"$max_age" \
    -exec sh -c 'echo "stale build (>'"$max_age"'d): $1"; rm -rf "$1"' _ {} \; 2>/dev/null || true
  git -C "$REPO_ROOT" worktree prune
  note "gc done — run 'list' to review remaining build-dir sizes"
}

case "${1:-}" in
  new)  shift; cmd_new "$@";;
  env)  shift; cmd_env "$@";;
  list) shift; cmd_list "$@";;
  gc)   shift; cmd_gc "$@";;
  *) sed -n '2,30p' "$0"; exit 1;;
esac
