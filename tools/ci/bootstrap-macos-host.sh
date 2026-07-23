#!/usr/bin/env bash
# bootstrap-macos-host.sh — provision a Mac as a Pulp CI runner host.
#
# Turns an Apple Silicon Mac (fresh or existing) into a Pulp self-hosted
# CI host: toolchain checks, Homebrew dependencies, the shared cache
# layout, ccache tuning, Skia verification, and Shipyard. Runner
# registration is a separate, explicit phase (--with-runners=N).
#
# Idempotent: safe to re-run.
#
# Usage:
#   tools/ci/bootstrap-macos-host.sh                  # deps + cache + verify
#   tools/ci/bootstrap-macos-host.sh --check          # report only, no changes
#   tools/ci/bootstrap-macos-host.sh --with-runners=2 # also register N runners
#
set -euo pipefail

# ── configuration ───────────────────────────────────────────────────────────
PINNED_XCODE_VERSION="${PULP_PINNED_XCODE:-26.5}"   # Xcode 26.5 (17F42)
CI_ROOT="${PULP_CI_ROOT:-/Users/Shared/pulp-ci}"
CCACHE_MAX_SIZE="${PULP_CCACHE_MAX_SIZE:-200G}"
REPO_SLUG="${PULP_REPO_SLUG:-Generous-Corp/pulp}"
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
  # Set the global default cache_dir (for ccache invocations with no
  # CCACHE_DIR env). The tuned knobs must additionally be written INTO
  # $CCACHE_DIR/ccache.conf — that is the config a runner job reads,
  # because the runner .env exports CCACHE_DIR. Writing only the global
  # config was the bug fixed here: jobs were silently capped at ccache's
  # 5 GB default.
  do_ ccache --set-config "cache_dir=$newdir"
  # Cross-worktree cache hits + correctness, written into the SHARED config so
  # env-less ccache invocations inherit them (not just runner jobs that carry
  # the per-runner .env). A CI host runs many worktrees off one .git, so with
  # no path normalization every worktree is a COLD cache by construction:
  #   - base_dir → rewrite absolute source paths under the CI work root to
  #     relative, so a hit compiled in one workspace serves another. Derived as
  #     the PARENT of the runner work root ($CI_ROOT/tmp/<name>) — never a
  #     hardcoded user home, so it is correct on any host.
  #   - hash_dir=false → keep the build CWD out of the hash (pairs with base_dir).
  #   - compiler_check=content + depend_mode=false → the #3504 correctness combo.
  #     Depend mode with mtime compiler keying on a cache shared across worktrees
  #     serves a stale/false-hit object that corrupts unrelated TUs (a pure
  #     function returns "" and change-unrelated tests fail). build.yml forces
  #     the same combo via job env for the GH-runner lane; this is its durable
  #     twin for every other consumer of the shared cache. Depend mode stays OFF
  #     fleet-wide.
  local work_root="$CI_ROOT/tmp" base_dir
  base_dir="$(dirname "$work_root")"
  for kv in \
    "max_size=$CCACHE_MAX_SIZE" \
    "compression=true" \
    "inode_cache=true" \
    "base_dir=$base_dir" \
    "hash_dir=false" \
    "compiler_check=content" \
    "depend_mode=false"; do
    do_ env CCACHE_DIR="$newdir" ccache --set-config "$kv"
  done
  ok "ccache: cache_dir=$newdir, max_size=$CCACHE_MAX_SIZE, base_dir=$base_dir, hash_dir=false, compiler_check=content, depend_mode=false (in \$CCACHE_DIR/ccache.conf)"
  note "per-runner CCACHE_BASEDIR/CCACHE_NOHASHDIR still layer on top in the --with-runners phase"
}

# ── Skia ─────────────────────────────────────────────────────────────────────
verify_skia() {
  step "Skia prebuilt binaries"
  # FindSkia.cmake needs the real static lib here; the LFS-declared Skia
  # binaries are not committed, so a fresh checkout has only an LFS
  # pointer. An LFS pointer file begins with "version https://git-lfs".
  local lib="$REPO_ROOT/external/skia-build/build/mac-gpu/lib/Release/libskia.a"
  local have=0
  if [ -f "$lib" ] && ! head -c 64 "$lib" 2>/dev/null | grep -qa "git-lfs"; then
    have=1
    ok "real Skia present ($lib)"
  fi
  if [ "$have" = 0 ]; then
    note "Skia missing or LFS-pointer-only — fetching the pinned release asset"
    if [ "$CHECK_ONLY" = 1 ]; then
      note "would run: python3 tools/scripts/fetch_skia_for_release.py darwin-arm64"
      return
    fi
    ( cd "$REPO_ROOT" && python3 tools/scripts/fetch_skia_for_release.py darwin-arm64 ) \
      || die "fetch_skia_for_release.py failed — cannot provision Skia for a GPU-capable host"
    if [ -f "$lib" ] && ! head -c 64 "$lib" 2>/dev/null | grep -qa "git-lfs"; then
      ok "Skia fetched + validated"
    else
      die "Skia fetch ran but $lib is still missing/pointer — a GPU build would fail"
    fi
  fi
  # Retain a real copy in the shared cache for fast re-provisioning.
  if [ -z "$(ls -A "$CI_ROOT/cache/skia-build" 2>/dev/null || true)" ]; then
    do_ rsync -a "$REPO_ROOT/external/skia-build/" "$CI_ROOT/cache/skia-build/"
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
CCACHE_MAXSIZE=$CCACHE_MAX_SIZE
CCACHE_BASEDIR=$work
CCACHE_NOHASHDIR=true
CCACHE_DEPEND=true
CCACHE_SLOPPINESS=time_macros,pch_defines
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
  if [ "$RUNNERS" = "0" ]; then
    note "runner registration skipped — re-run with --with-runners=N"
  fi
}
main
exit 0
