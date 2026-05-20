#!/usr/bin/env bash
# bootstrap-macos-host.sh — provision a Mac as a Pulp CI runner host.
#
# Turns an Apple Silicon Mac (fresh or existing) into a Pulp self-hosted
# CI host: toolchain checks, Homebrew dependencies, the shared cache
# layout, ccache tuning, Skia verification, and Shipyard. Runner
# registration is a separate, explicit phase (--with-runners=N).
#
# Idempotent: safe to re-run. Plan + rationale:
#   planning/2026-05-20-macos-ci-modernization-proposal.md
#
# Usage:
#   tools/ci/bootstrap-macos-host.sh                  # deps + cache + verify
#   tools/ci/bootstrap-macos-host.sh --check          # report only, no changes
#   tools/ci/bootstrap-macos-host.sh --with-runners=2 # also register N runners
#
set -euo pipefail

# ── configuration ───────────────────────────────────────────────────────────
PINNED_XCODE_VERSION="${PULP_PINNED_XCODE:-26.4.1}"   # Xcode 26.4.1 (17E202)
CI_ROOT="${PULP_CI_ROOT:-/Users/Shared/pulp-ci}"
CCACHE_MAX_SIZE="${PULP_CCACHE_MAX_SIZE:-80G}"
REPO_SLUG="${PULP_REPO_SLUG:-danielraffel/pulp}"
RUNNER_LABELS="${PULP_RUNNER_LABELS:-self-hosted,macos,arm64,pulp-build}"
# Runner name prefix; instances are named "<prefix>-NN". Use a machine
# tag (pulp-m1, pulp-m5, ...) so runners on different hosts stay
# distinct and a label like pulp-build-m1 can target one host.
RUNNER_NAME_PREFIX="${PULP_RUNNER_NAME_PREFIX:-pulp-$(hostname -s)}"

CHECK_ONLY=0
RUNNERS=0
for a in "$@"; do
  case "$a" in
    --check) CHECK_ONLY=1 ;;
    --with-runners=*) RUNNERS="${a#*=}" ;;
    -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "bootstrap-macos-host: unknown argument '$a'" >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

step() { printf '\n=== %s ===\n' "$1"; }
ok()   { printf '  [ok]   %s\n' "$1"; }
note() { printf '  [..]   %s\n' "$1"; }
warn() { printf '  [warn] %s\n' "$1" >&2; }
die()  { printf '  [FAIL] %s\n' "$1" >&2; exit 1; }
do_()  { if [ "$CHECK_ONLY" = 1 ]; then note "would run: $*"; else "$@"; fi; }

# ── preflight ────────────────────────────────────────────────────────────────
preflight() {
  step "Preflight"
  [ "$(uname -s)" = "Darwin" ] || die "not macOS"
  [ "$(uname -m)" = "arm64" ]  || die "not Apple Silicon (arm64)"
  command -v git >/dev/null    || die "git not found"
  ok "macOS / arm64 / git"
}

# ── Xcode ────────────────────────────────────────────────────────────────────
check_xcode() {
  step "Xcode"
  command -v xcodebuild >/dev/null || die "xcodebuild not found — install Xcode $PINNED_XCODE_VERSION"
  local v
  v="$(xcodebuild -version 2>/dev/null | awk 'NR==1{print $2}')"
  if [ "$v" = "$PINNED_XCODE_VERSION" ]; then
    ok "Xcode $v (matches pin)"
  else
    warn "Xcode $v installed; plan pins $PINNED_XCODE_VERSION — reconcile the pin or the toolchain"
  fi
  note "developer dir: $(xcode-select -p 2>/dev/null || echo '?')"
  if xcodebuild -license check >/dev/null 2>&1; then
    ok "Xcode license accepted"
  else
    warn "Xcode license not accepted — run: sudo xcodebuild -license accept"
  fi
}

# ── Homebrew dependencies ────────────────────────────────────────────────────
install_deps() {
  step "Homebrew dependencies"
  command -v brew >/dev/null || die "Homebrew not installed — install from https://brew.sh first"
  do_ brew bundle --file "$SCRIPT_DIR/Brewfile" --no-upgrade
  ok "Brewfile satisfied"
}

# ── shared cache + workspace layout ──────────────────────────────────────────
make_layout() {
  step "Cache layout ($CI_ROOT)"
  for d in cache/skia-build cache/fetchcontent-src cache/ccache tmp; do
    do_ mkdir -p "$CI_ROOT/$d"
  done
  ok "$CI_ROOT layout present"
}

# ── ccache tuning ────────────────────────────────────────────────────────────
tune_ccache() {
  step "ccache tuning"
  command -v ccache >/dev/null || die "ccache not found (expected from Brewfile)"
  local newdir="$CI_ROOT/cache/ccache" olddir
  olddir="$(ccache --get-config cache_dir 2>/dev/null || echo "$HOME/Library/Caches/ccache")"
  # Migrate an existing warm cache to the shared location exactly once.
  if [ "$olddir" != "$newdir" ] && [ -d "$olddir" ] \
     && [ -z "$(ls -A "$newdir" 2>/dev/null || true)" ]; then
    note "migrating warm cache: $olddir -> $newdir"
    do_ rsync -a "$olddir/" "$newdir/" || warn "cache migration incomplete (non-fatal — it rewarms)"
  fi
  do_ ccache --set-config "cache_dir=$newdir"
  do_ ccache --set-config "max_size=$CCACHE_MAX_SIZE"
  do_ ccache --set-config "compression=true"
  do_ ccache --set-config "inode_cache=true"
  ok "ccache: cache_dir=$newdir, max_size=$CCACHE_MAX_SIZE, compression on"
  note "cross-worktree path normalization (CCACHE_BASEDIR + CCACHE_NOHASHDIR)"
  note "is applied per runner workspace by the --with-runners phase"
}

# ── Skia ─────────────────────────────────────────────────────────────────────
verify_skia() {
  step "Skia prebuilt binaries"
  local repo_skia="$REPO_ROOT/external/skia-build"
  local cache_skia="$CI_ROOT/cache/skia-build"
  if [ -d "$repo_skia" ] && [ -n "$(ls -A "$repo_skia" 2>/dev/null || true)" ]; then
    ok "external/skia-build present"
    if [ -z "$(ls -A "$cache_skia" 2>/dev/null || true)" ]; then
      do_ rsync -a "$repo_skia/" "$cache_skia/" \
        && note "retained a copy in $cache_skia for fast re-provisioning"
    fi
  elif [ -n "$(ls -A "$cache_skia" 2>/dev/null || true)" ]; then
    note "seeding external/skia-build from retained cache copy"
    do_ rsync -a "$cache_skia/" "$repo_skia/"
  else
    warn "external/skia-build missing — GPU build will be disabled."
    warn "Seed it: 'git lfs pull' in the repo, or copy from another host."
  fi
}

# ── Shipyard ─────────────────────────────────────────────────────────────────
install_shipyard() {
  step "Shipyard"
  if [ -x "$REPO_ROOT/tools/install-shipyard.sh" ]; then
    do_ "$REPO_ROOT/tools/install-shipyard.sh"
    ok "Shipyard installed / pinned"
  else
    warn "tools/install-shipyard.sh not found — skipping"
  fi
}

# ── verify build dependencies ────────────────────────────────────────────────
verify_build() {
  step "Verify build dependencies"
  if [ "$CHECK_ONLY" = 1 ]; then note "would run: ./setup.sh --ci --deps-only"; return; fi
  if ( cd "$REPO_ROOT" && ./setup.sh --ci --deps-only ); then
    ok "setup.sh --ci --deps-only succeeded"
  else
    die "setup.sh --ci --deps-only FAILED — host is NOT provisioned. Fix the dependency errors above and re-run; do not register runners on this host until this passes."
  fi
}

# ── runner registration (A-2 phase — explicit opt-in) ────────────────────────
register_runners() {
  step "Register $RUNNERS GitHub Actions runner(s)"
  command -v gh >/dev/null || die "gh CLI required for runner registration"
  local cores per
  cores="$(sysctl -n hw.ncpu)"
  per=$(( cores / RUNNERS )); [ "$per" -lt 1 ] && per=1
  note "host has $cores cores -> CMAKE_BUILD_PARALLEL_LEVEL=$per, CTEST_PARALLEL_LEVEL=$per per runner"
  local pkg_url pkg
  pkg_url="$(gh api repos/actions/runner/releases/latest \
    --jq '.assets[] | select(.name | test("osx-arm64.*tar.gz$")) | .browser_download_url' 2>/dev/null || true)"
  [ -n "$pkg_url" ] || die "could not resolve the actions-runner osx-arm64 package URL"
  pkg="$(basename "$pkg_url")"
  for i in $(seq 1 "$RUNNERS"); do
    local idx name rdir work
    idx="$(printf '%02d' "$i")"
    name="$RUNNER_NAME_PREFIX-$idx"
    # Per-runner dir + workspace keyed on the unique runner name, so
    # multiple runners (and runners for other repos already on the host)
    # never collide or overwrite each other.
    rdir="$HOME/actions-runner-$name"
    work="$CI_ROOT/tmp/$name"
    note "runner $idx: name=$name dir=$rdir work=$work"
    do_ mkdir -p "$rdir" "$work"
    if [ ! -x "$rdir/config.sh" ]; then
      do_ bash -c "cd '$rdir' && curl -fsSL -o '$pkg' '$pkg_url' && tar xzf '$pkg' && rm -f '$pkg'"
    fi
    # Per-runner service environment: ccache + parallelism + FetchContent.
    do_ bash -c "cat > '$rdir/.env' <<ENV
CCACHE_DIR=$CI_ROOT/cache/ccache
CCACHE_BASEDIR=$work
CCACHE_NOHASHDIR=true
CMAKE_BUILD_PARALLEL_LEVEL=$per
CTEST_PARALLEL_LEVEL=$per
FETCHCONTENT_BASE_DIR=$CI_ROOT/cache/fetchcontent-src
ENV"
    if [ "$CHECK_ONLY" = 1 ]; then
      note "would register $name and install the launchd service"
      continue
    fi
    local token
    token="$(gh api -X POST "repos/$REPO_SLUG/actions/runners/registration-token" --jq '.token')"
    ( cd "$rdir" && ./config.sh --unattended --replace \
        --url "https://github.com/$REPO_SLUG" --token "$token" \
        --name "$name" --labels "$RUNNER_LABELS" --work "$work" )
    ( cd "$rdir" && ./svc.sh install && ./svc.sh start )
    ok "runner $name registered + service started"
  done
  warn "to retire a runner: 'cd <rdir> && ./svc.sh stop && ./config.sh remove --token <removal-token>'"
}

# ── main ─────────────────────────────────────────────────────────────────────
main() {
  printf 'Pulp macOS CI host bootstrap'
  [ "$CHECK_ONLY" = 1 ] && printf ' (--check: report only)'
  printf '\n  repo: %s\n  host: %s\n' "$REPO_ROOT" "$(hostname -s)"
  preflight
  check_xcode
  install_deps
  make_layout
  tune_ccache
  verify_skia
  install_shipyard
  verify_build
  if [ "$RUNNERS" -gt 0 ]; then register_runners; fi
  step "Done"
  ok "host provisioned"
  [ "$RUNNERS" = "0" ] && note "runner registration skipped — re-run with --with-runners=N"
}
main
