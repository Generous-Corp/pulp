#!/usr/bin/env bash
# tart-run-job.sh — run ONE Pulp build+test inside an ephemeral Tart VM cloned
# from a golden master, with the durable caches mounted from the host, then
# discard the clone. This is how the VM pool "takes over the build load"
# while build dirs stay disposable and ccache/Skia/FetchContent stay
# host-durable and shared across clones.
#
# Flow:
#   tart clone <golden> <job-vm>          # CoW, seconds
#   tart run --dir=ccache:… --dir=skia:…  # virtio-fs host mounts
#   ssh → build + ctest in-guest          # SKIA_DIR + ccache point at mounts
#   tart stop && tart delete <job-vm>      # discard (unless --keep)
#
# virtio-fs notes:
#   • Tart mounts a --dir=<name>:<path> at "/Volumes/My Shared Files/<name>".
#     We symlink each into $HOME to dodge the space in the path.
#   • ccache must keep its TEMPDIR IN-GUEST (CCACHE_TEMPDIR) — cross-fs renames
#     onto a virtio-fs mount break ccache. CCACHE_BASEDIR normalizes abs paths.
#   • Same UID/GID (guest admin uid 501 == host primary user) keeps the shared
#     ccache writable both ways. Stress-test two concurrent clones before
#     relying on a single shared ccache (race-safe: ccache uses atomic renames).
#
# Usage:
#   tart-run-job.sh --golden pulp-build-base:latest --src /path/to/checkout \
#       [--vm job-NAME] [--disk 150] [--build-type Release] \
#       [--cache-root ~/.cache/pulp-ci] [--ctest-args "..."] [--keep]
set -euo pipefail

# TART_HOME comes from the host, not from this repo — see tools/ci/lib/tart-home.sh.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/tart-home.sh"

SSH_KEY_PRIV="${PULP_VM_SSH_KEY:-$HOME/.ssh/id_ed25519}"
VM_USER="${PULP_VM_USER:-admin}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10)

GOLDEN=""; SRC=""; VM=""; DISK=""; BUILD_TYPE="Release"
CACHE_ROOT="${PULP_CI_CACHE:-$HOME/.cache/pulp-ci}"
CTEST_ARGS="--output-on-failure --exclude-regex AudioWorkgroup --label-exclude slow"
KEEP=0

note(){ printf '\033[36m• %s\033[0m\n' "$*" >&2; }
die(){ printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }
pulp_require_tart_home
command -v tart >/dev/null 2>&1 || die "tart not installed — brew install cirruslabs/cli/tart"

while [ $# -gt 0 ]; do case "$1" in
  --golden) GOLDEN="$2"; shift 2;;
  --src) SRC="$2"; shift 2;;
  --vm) VM="$2"; shift 2;;
  --disk) DISK="$2"; shift 2;;
  --build-type) BUILD_TYPE="$2"; shift 2;;
  --cache-root) CACHE_ROOT="$2"; shift 2;;
  --ctest-args) CTEST_ARGS="$2"; shift 2;;
  --keep) KEEP=1; shift;;
  *) die "unknown arg: $1";;
esac; done

[ -n "$GOLDEN" ] || die "--golden <image> required (e.g. pulp-build-base:latest)"
[ -n "$SRC" ] && [ -d "$SRC" ] || die "--src <checkout-dir> required and must exist"
SRC="$(cd "$SRC" && pwd)"
# A unique-but-deterministic-per-invocation name. Caller can pass --vm; default
# uses the parent shell PID (no Date.now/rand needed and unique enough per run).
VM="${VM:-job-$$}"
mkdir -p "$CACHE_ROOT"/{ccache,skia-build,fetchcontent-src}

cleanup(){
  [ "$KEEP" = 1 ] && { note "--keep: leaving $VM (delete with: tart delete $VM)"; return; }
  tart stop "$VM" >/dev/null 2>&1 || true
  [ -n "${RPID:-}" ] && kill "$RPID" 2>/dev/null || true
  sleep 2
  tart delete "$VM" >/dev/null 2>&1 || true
  note "discarded ephemeral VM $VM"
}
trap cleanup EXIT

note "cloning $GOLDEN → $VM (CoW)"
tart clone "$GOLDEN" "$VM"
if [ -n "$DISK" ]; then tart set "$VM" --disk-size "$DISK"; note "disk → ${DISK}G (guest-agent grows APFS on boot)"; fi

note "booting with host mounts (src ro, ccache/fetchcontent rw); Skia is baked into the golden"
tart run --no-graphics \
  --dir="src:$SRC:ro" \
  --dir="ccache:$CACHE_ROOT/ccache" \
  --dir="fetchcontent:$CACHE_ROOT/fetchcontent-src" \
  "$VM" >/dev/null 2>&1 & RPID=$!

# Wait for IP + ssh.
IP=""; for i in $(seq 1 60); do IP="$(tart ip "$VM" 2>/dev/null || true)"; [ -n "$IP" ] && break; sleep 2; done
[ -n "$IP" ] || die "no IP for $VM"
for i in $(seq 1 90); do ssh "${SSH_OPTS[@]}" -i "$SSH_KEY_PRIV" "$VM_USER@$IP" true 2>/dev/null && break; sleep 2; done
note "vm $VM up at $IP"

vmsh(){ ssh "${SSH_OPTS[@]}" -i "$SSH_KEY_PRIV" "$VM_USER@$IP" "$@"; }

# The in-guest build script. Symlink the space-laden mount paths into $HOME,
# point caches at them, build into an in-guest dir, run ctest. Exit code
# propagates back through ssh.
note "running build ($BUILD_TYPE) + ctest in-guest"
set +e
vmsh "BUILD_TYPE='$BUILD_TYPE' CTEST_ARGS='$CTEST_ARGS' bash -s" <<'GUEST'
set -euo pipefail
eval "$(/opt/homebrew/bin/brew shellenv)"
SHARED="/Volumes/My Shared Files"
ln -sfn "$SHARED/src" "$HOME/src"
ln -sfn "$SHARED/ccache" "$HOME/ccache"
ln -sfn "$SHARED/fetchcontent" "$HOME/fetchcontent"

export CCACHE_DIR="$HOME/ccache"
export CCACHE_TEMPDIR="$HOME/.ccache-tmp"        # IN-GUEST — never on the virtio-fs mount
mkdir -p "$CCACHE_TEMPDIR"
export CCACHE_BASEDIR="$HOME/src"
export CCACHE_NOHASHDIR=true
export CCACHE_SLOPPINESS=time_macros,pch_defines
export CCACHE_DEPEND=true
# Skia is baked into the pulp-build-base golden at ~/pulp-skia-build (Tier 2).
# FindSkia.cmake wants the dir CONTAINING build/, not .../build/mac-gpu.
export SKIA_DIR="$HOME/pulp-skia-build"
export FETCHCONTENT_BASE_DIR="$HOME/fetchcontent"

ccache --zero-stats >/dev/null 2>&1 || true
BUILD="$HOME/build"
rm -rf "$BUILD"
cmake -S "$HOME/src" -B "$BUILD" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DPULP_BUILD_TESTS=ON -DPULP_BUILD_EXAMPLES=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build "$BUILD" --parallel "$(sysctl -n hw.ncpu)"
echo "=== ccache stats (warmth) ==="
ccache --show-stats | grep -iE 'cacheable|hit|miss|cache size' || ccache -s
ctest --test-dir "$BUILD" $CTEST_ARGS
GUEST
RC=$?
set -e

note "in-guest exit=$RC"
echo "=== host ccache after job (durable across clones) ==="
CCACHE_DIR="$CACHE_ROOT/ccache" ccache --show-stats 2>/dev/null | grep -iE 'cache size|hits|misses|hit rate' || true
exit $RC
