#!/usr/bin/env bash
# intel-vm-cross-build.sh — build (and Rosetta-run) the Pulp x86_64 CLI inside a
# throwaway ARM Tart VM, so occasional Intel builds/tests never touch the host.
#
# WHY: Pulp's macOS release leg for Intel (`darwin-x64`) is native on GitHub's
# `macos-15-intel`, which is slow + nightly-gated. For an interactive "does the
# Intel build still work?" check, an Apple-Silicon host can CROSS-compile x86_64
# and run it under Rosetta 2 — no native Intel hardware needed, no x86_64
# toolchain installed on the host. This clones the `pulp-intel-build` golden
# (Rust + x86_64-apple-darwin target + Rosetta baked in), runs the proven
# cross-build recipe on a git ref, verifies (exact-thin x86_64 + Rosetta run),
# and discards the VM.
#
# USAGE:
#   tools/ci/intel-vm-cross-build.sh [--ref <branch|tag|sha>] [--golden <img>]
#                                    [--keep] [--full-build]
#   --ref         git ref to build (default: main)
#   --golden      golden image tag   (default: pulp-intel-build:latest)
#   --keep        don't delete the VM afterward (leaves it running for triage)
#   --full-build  `cmake --build` everything (SDK-faithful, slower). Default
#                 builds just the CLI target chain (pulp/pulp-cpp/pulp-mcp).
#
# REQUIRES: tart, and the golden image present on this host (see
# `.shipyard/vm-image.intel.toml` + `tools/ci/tart-provision.sh manifest`).
# Guest creds default to admin/admin via sshpass, or key auth if injected.
set -euo pipefail

REF="main"
GOLDEN="pulp-intel-build:latest"
KEEP=0
FULL_BUILD=0
REPO_URL="https://github.com/danielraffel/pulp.git"

while [ $# -gt 0 ]; do
  case "$1" in
    --ref)        REF="$2"; shift 2;;
    --golden)     GOLDEN="$2"; shift 2;;
    --keep)       KEEP=1; shift;;
    --full-build) FULL_BUILD=1; shift;;
    -h|--help)    sed -n '2,30p' "$0"; exit 0;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

command -v tart >/dev/null 2>&1 || { echo "ERROR: tart not installed (brew install cirruslabs/cli/tart)"; exit 3; }
tart list 2>/dev/null | grep -q "${GOLDEN%%:*}" || { echo "ERROR: golden '$GOLDEN' not found on this host. Bake/replicate it first (see .shipyard/vm-image.intel.toml)."; exit 3; }

# Deterministic-ish ephemeral name (no Math.random needed; PID + ref).
VM="pulp-intel-xbuild-$$"
IP=""
RUN_PID=""

cleanup() {
  [ "$KEEP" = "1" ] && { echo "[keep] leaving VM '$VM' (ip=${IP:-?}); delete with: tart delete $VM"; return; }
  [ -n "$RUN_PID" ] && kill "$RUN_PID" 2>/dev/null || true
  tart stop "$VM" >/dev/null 2>&1 || true
  tart delete "$VM" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> cloning golden $GOLDEN → $VM"
tart clone "$GOLDEN" "$VM"

echo "==> booting $VM (headless)"
tart run "$VM" --no-graphics >/dev/null 2>&1 &
RUN_PID=$!

echo "==> waiting for VM IP"
for _ in $(seq 1 40); do IP="$(tart ip "$VM" 2>/dev/null || true)"; [ -n "$IP" ] && break; sleep 5; done
[ -n "$IP" ] || { echo "ERROR: VM never got an IP"; exit 4; }
echo "    ip=$IP"

# ssh helper: key auth if it works, else sshpass admin/admin.
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=20 -o UserKnownHostsFile=/dev/null"
vmssh() {
  if ssh $SSH_OPTS "admin@$IP" true 2>/dev/null; then ssh $SSH_OPTS "admin@$IP" "$@";
  else sshpass -p admin ssh $SSH_OPTS "admin@$IP" "$@"; fi
}
vmscp() {
  if ssh $SSH_OPTS "admin@$IP" true 2>/dev/null; then scp $SSH_OPTS "$1" "admin@$IP:$2";
  else sshpass -p admin scp $SSH_OPTS "$1" "admin@$IP:$2"; fi
}

echo "==> waiting for sshd"
for _ in $(seq 1 24); do vmssh true 2>/dev/null && break; sleep 5; done

BUILD_TARGETS=""
[ "$FULL_BUILD" = "0" ] && BUILD_TARGETS="--target pulp-rust-cli pulp-cpp pulp-mcp"

# The recipe runs in-guest. Note the Skia-clobber fix (REQUIRED on a cross build):
# setup.sh --ci prefetches the HOST-arch (arm64) Skia and fetch_skia's flatten
# step won't clobber it, so the x86_64 FindSkia fails unless we clear it first.
RECIPE=$(cat <<REMOTE
set -euo pipefail
export PATH=\$HOME/.cargo/bin:/opt/homebrew/bin:/usr/local/bin:\$PATH
echo "[vm] arch=\$(uname -m) cargo=\$(cargo --version 2>/dev/null) x86_64-target=\$(rustup target list --installed 2>/dev/null | grep x86_64-apple-darwin || echo MISSING)"
[ -n "\$(rustup target list --installed 2>/dev/null | grep x86_64-apple-darwin)" ] || { echo "FAIL: golden lacks x86_64-apple-darwin rust target"; exit 11; }
rm -rf ~/pulp-xbuild && git clone --depth 1 --branch "$REF" "$REPO_URL" ~/pulp-xbuild
cd ~/pulp-xbuild
./setup.sh --ci --deps-only
rm -rf external/skia-build/build   # cross-build Skia-clobber fix
python3 tools/scripts/fetch_skia_for_release.py darwin-x64
[ "\$(lipo -archs external/skia-build/build/mac-gpu/lib/Release/libskia.a 2>/dev/null)" = "x86_64" ] || { echo "FAIL: skia not x86_64 after clean fetch"; exit 12; }
env -u SKIA_DIR cmake -S . -B build-x64 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DPULP_RUST_CLI_TARGET=x86_64-apple-darwin \
  -DPULP_BUILD_TESTS=OFF -DPULP_ENABLE_AUDIO_PROBES=OFF \
  -DPULP_REQUIRE_GPU_FOR_SDK=ON -DPULP_SKIA_AUTOFETCH=ON -DPULP_BUILD_WEBVIEW=ON
cmake --build build-x64 --config Release $BUILD_TARGETS
BIN=build-x64/pulp
[ -x "\$BIN" ] || { echo "FAIL: \$BIN missing"; exit 13; }
ARCHS=\$(lipo -archs "\$BIN")
[ "\$ARCHS" = "x86_64" ] || { echo "FAIL: pulp is '\$ARCHS', expected thin x86_64"; exit 14; }
arch -x86_64 "\$BIN" help >/dev/null 2>&1 || arch -x86_64 "\$BIN" --version >/dev/null 2>&1 || { echo "FAIL: pulp did not run under Rosetta"; exit 15; }
echo "pulp=\$ARCHS pulp-cpp=\$(lipo -archs build-x64/tools/cli/pulp-cpp 2>/dev/null) wgpu=\$(find build-x64 -name 'libwgpu_native*.dylib' | head -1 | xargs lipo -archs 2>/dev/null)"
echo "INTEL-BUILD-VERIFIED-OK"
REMOTE
)

echo "==> running cross-build recipe in VM (ref=$REF, full=$FULL_BUILD)"
if vmssh "bash -lc '$RECIPE'" 2>&1 | tee /tmp/intel-vm-build-$$.log | grep -q "INTEL-BUILD-VERIFIED-OK"; then
  echo "==> ✅ INTEL BUILD VERIFIED (ref=$REF)"
  exit 0
else
  echo "==> ❌ INTEL BUILD FAILED (ref=$REF) — see /tmp/intel-vm-build-$$.log" >&2
  exit 1
fi
