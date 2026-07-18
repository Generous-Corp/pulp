#!/usr/bin/env bash
# tart-provision.sh — build/refresh layered "golden master" Tart VMs for CI,
# and resize them. Encodes the tier checklist as reproducible steps, so a new
# golden master is `tart-provision <tier>` rather than hand-clicking.
#
# Images (date-tag with the `tag` subcommand):
#   macos-build-base   Tier 0: sshd+key, auto-login, brew(git gh rsync ccache jq
#                              coreutils tailscale), runner agent           ~50 GB
#   macos-apple-xcode  Tier 1: + Xcode 26.5 (17F42) + CLT, sim runtimes trimmed ~90 GB
#   pulp-build-base    Tier 2: + cmake ninja python@3.12(PIL,numpy) + Skia +
#                              warm ccache/FetchContent                   ~110-130 GB
#
# Storage: the host's TART_HOME (Spotlight-excluded). Clones are CoW.
#
# Verified facts (cirruslabs macos-image-templates, tart docs — 2026-06-01):
#   • Base image  ghcr.io/cirruslabs/macos-tahoe-base:latest is macOS 26 "Tahoe",
#     which matches Xcode 26.5's required macOS.
#   • Default creds are admin / admin.
#   • The vanilla base ALREADY enables auto-login (kcpassword + loginwindow
#     autoLoginUser=admin) and Remote Login (sshd). We verify/inject, not re-create.
#   • `tart set <vm> --disk-size N` only grows, and only on a STOPPED VM; the
#     bundled tart-guest-agent grows the APFS container on next boot. Manual
#     fallback: `diskutil apfs resizeContainer disk0s2 0` over ssh.
#   • Xcode: re-downloading in-guest re-triggers Apple-ID/2FA and a multi-hour
#     fetch. Prefer rsync'ing a host-installed Xcode (PULP_HOST_XCODE_APP);
#     `xcodes install` is the fallback and needs interactive auth.
set -euo pipefail

# TART_HOME comes from the host, not from this repo — see tools/ci/lib/tart-home.sh.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/tart-home.sh"

SSH_KEY_PUB="${PULP_VM_SSH_KEY_PUB:-$HOME/.ssh/id_ed25519.pub}"
SSH_KEY_PRIV="${SSH_KEY_PUB%.pub}"
# Tahoe matches Xcode 26.5. Override with PULP_MACOS_BASE_IMAGE if Apple ships a
# newer macOS for a future Xcode.
BASE_MACOS_IMAGE="${PULP_MACOS_BASE_IMAGE:-ghcr.io/cirruslabs/macos-tahoe-base:latest}"
XCODE_VERSION="${PULP_XCODE_VERSION:-26.5}"          # build 17F42 — goldens are toolchain-coupled
# xcodes installs to /Applications/Xcode-<version>.app; on the host, an already
# xcodes-installed 26.5 is typically /Applications/Xcode-26.5.0.app.
XCODE_APP="${PULP_XCODE_APP:-/Applications/Xcode-${XCODE_VERSION}.0.app}"
HOST_XCODE_APP="${PULP_HOST_XCODE_APP:-}"            # if set, rsync this host Xcode in (no re-download)
VM_USER="${PULP_VM_USER:-admin}"; VM_PASS="${PULP_VM_PASS:-admin}"  # Cirrus base default
DISK_GB="${PULP_VM_DISK_GB:-150}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10)

note(){ printf '\033[36m• %s\033[0m\n' "$*" >&2; }
warn(){ printf '\033[33m⚠ %s\033[0m\n' "$*" >&2; }
die(){ printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }
# Every tart-touching subcommand goes through here, so it is also where the VM
# store gets resolved — no subcommand can reach `tart` with an unresolved store.
need_tart(){
  pulp_require_tart_home
  command -v tart >/dev/null 2>&1 || die "tart not installed — brew install cirruslabs/cli/tart"
}
have_sshpass(){ command -v sshpass >/dev/null 2>&1; }

# Run a command inside a booted VM over ssh. Prefer key auth (present after Tier
# 0 injection); fall back to password auth via sshpass for the very first
# bootstrap connection. sshpass: brew install hudochenkov/sshpass/sshpass
vm_ssh(){
  local ip="$1"; shift
  if ssh "${SSH_OPTS[@]}" -o BatchMode=yes -i "$SSH_KEY_PRIV" "$VM_USER@$ip" true 2>/dev/null; then
    ssh "${SSH_OPTS[@]}" -i "$SSH_KEY_PRIV" "$VM_USER@$ip" "$@"
  elif have_sshpass; then
    sshpass -p "$VM_PASS" ssh "${SSH_OPTS[@]}" "$VM_USER@$ip" "$@"
  else
    die "no key auth yet and sshpass missing — brew install hudochenkov/sshpass/sshpass"
  fi
}
vm_rsync(){  # vm_rsync <src> <ip>:<dest> [extra rsync opts...]
  local src="$1" dst="$2"; shift 2
  rsync -a --delete "$@" -e "ssh ${SSH_OPTS[*]} -i $SSH_KEY_PRIV" "$src" "$VM_USER@$dst"
}

# Boot a VM headless and wait until ssh answers. Echoes the IP. Caller keeps the
# returned pid to stop later. Usage:  rpid=$(boot_vm "$vm"); ip=$(vm_ip "$vm")
boot_vm(){ # $1=vm  -> prints run-pid on stdout
  local vm="$1"
  tart run --no-graphics "$vm" >/dev/null 2>&1 & local rpid=$!
  echo "$rpid"
}
vm_ip(){ # $1=vm — poll up to ~120s for an IP
  local vm="$1" ip="" i
  for i in $(seq 1 60); do
    ip="$(tart ip "$vm" 2>/dev/null || true)"
    [ -n "$ip" ] && { echo "$ip"; return 0; }
    sleep 2
  done
  die "timed out waiting for IP of $vm"
}
wait_ssh(){ # $1=ip — poll up to ~180s for ssh to answer (key OR password)
  local ip="$1" i
  for i in $(seq 1 90); do
    if vm_ssh "$ip" true 2>/dev/null; then return 0; fi
    sleep 2
  done
  die "timed out waiting for ssh on $ip"
}
stop_vm(){ # graceful stop, fall back to killing the run pid
  local vm="$1" rpid="${2:-}"
  tart stop "$vm" >/dev/null 2>&1 || true
  [ -n "$rpid" ] && kill "$rpid" 2>/dev/null || true
  # give the VZ process a moment to release the disk image
  sleep 3
}

# ── Tier 0 — universal base ────────────────────────────────────────────────
provision_base(){ # $1=vm-name
  need_tart; local vm="${1:-macos-build-base}"
  [ -f "$SSH_KEY_PUB" ] || die "ssh pubkey not found: $SSH_KEY_PUB (set PULP_VM_SSH_KEY_PUB)"
  note "Tier 0: cloning $BASE_MACOS_IMAGE → $vm"
  tart clone "$BASE_MACOS_IMAGE" "$vm"
  note "growing disk → ${DISK_GB}G (guest-agent grows APFS on boot)"
  tart set "$vm" --disk-size "$DISK_GB"            # VM is stopped here — required
  local rpid; rpid="$(boot_vm "$vm")"
  local ip; ip="$(vm_ip "$vm")"; note "vm ip=$ip"
  wait_ssh "$ip"
  # Inject our key (first connection uses password via sshpass).
  note "injecting ssh key"
  vm_ssh "$ip" "mkdir -p ~/.ssh && chmod 700 ~/.ssh && grep -qxF '$(cat "$SSH_KEY_PUB")' ~/.ssh/authorized_keys 2>/dev/null || echo '$(cat "$SSH_KEY_PUB")' >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys"
  vm_ssh "$ip" 'sudo systemsetup -setremotelogin on >/dev/null 2>&1 || true'   # idempotent; base already on
  # Auto-login is set by the cirrus vanilla base. Verify; warn (don't silently
  # regenerate kcpassword) if a non-cirrus base lacks it — GPU/WindowServer/Metal
  # tests REQUIRE a logged-in session.
  local autouser; autouser="$(vm_ssh "$ip" 'defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser 2>/dev/null || true')"
  if [ "$autouser" = "$VM_USER" ]; then note "auto-login OK (autoLoginUser=$autouser)"
  else warn "auto-login NOT set to $VM_USER (got '${autouser:-none}') — Metal/GPU tests need it; configure kcpassword + loginwindow autoLoginUser before GPU probe"; fi
  # Homebrew + common CI tooling (+ tailscale for mesh SSH to the VM).
  vm_ssh "$ip" 'command -v brew >/dev/null || NONINTERACTIVE=1 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
  vm_ssh "$ip" 'eval "$(/opt/homebrew/bin/brew shellenv)" && brew install --quiet git gh rsync ccache jq coreutils tailscale'
  note "Tier 0 provisioned. Operator: run 'tailscale up' in-VM to authenticate the mesh; install the GH runner agent if this VM will self-host."
  stop_vm "$vm" "$rpid"
  note "Tier 0 ready. Tag it:  $0 tag $vm macos-build-base"
}

# ── Tier 1 — Xcode (Apple projects) ────────────────────────────────────────
provision_apple_xcode(){ # $1=from-base-vm  $2=new-vm
  need_tart; local from="${1:-macos-build-base}" vm="${2:-macos-apple-xcode}"
  note "Tier 1: cloning $from → $vm"
  tart clone "$from" "$vm"
  local rpid; rpid="$(boot_vm "$vm")"
  local ip; ip="$(vm_ip "$vm")"; wait_ssh "$ip"
  if [ -n "$HOST_XCODE_APP" ] && [ -d "$HOST_XCODE_APP" ]; then
    # Fast path: copy the operator's already-downloaded Xcode (no re-auth/re-download).
    local appname; appname="$(basename "$HOST_XCODE_APP")"
    note "rsync host Xcode $HOST_XCODE_APP → VM:/Applications/$appname (no re-download)"
    # Remote rsync runs as root (passwordless sudo) so it can write /Applications
    # and preserve root ownership; full path pins brew rsync on both ends so the
    # protocol matches (macOS 26 ships openrsync at /usr/bin/rsync).
    vm_rsync "$HOST_XCODE_APP/" "$ip:/Applications/$appname/" --rsync-path="sudo /opt/homebrew/bin/rsync"
    vm_ssh "$ip" "sudo xcode-select -s /Applications/$appname/Contents/Developer"
  else
    warn "PULP_HOST_XCODE_APP unset — installing Xcode $XCODE_VERSION in-guest via xcodes (needs Apple-ID + interactive 2FA; multi-hour download)"
    vm_ssh "$ip" 'eval "$(/opt/homebrew/bin/brew shellenv)" && brew install --quiet xcodesorg/made/xcodes aria2'
    # XCODES_USERNAME/XCODES_PASSWORD may be exported to reduce prompts; 2FA is
    # still interactive. Run this attached if auth is needed.
    vm_ssh "$ip" "eval \"\$(/opt/homebrew/bin/brew shellenv)\" && xcodes install '$XCODE_VERSION' --experimental-unxip --no-superuser || true"
    vm_ssh "$ip" "sudo xcode-select -s '$XCODE_APP/Contents/Developer'"
  fi
  vm_ssh "$ip" 'sudo xcodebuild -license accept && sudo xcodebuild -runFirstLaunch' || warn "xcodebuild license/firstlaunch step needs attention"
  # Trim simulator runtimes (large; CI plugin builds don't need them).
  vm_ssh "$ip" 'for r in $(xcrun simctl runtime list -j 2>/dev/null | jq -r "keys[]" 2>/dev/null); do xcrun simctl runtime delete "$r" 2>/dev/null || true; done'
  note "Tier 1 provisioned."
  stop_vm "$vm" "$rpid"
  note "Tier 1 ready. Tag it:  $0 tag $vm macos-apple-xcode"
}

# ── Tier 2 — Pulp ──────────────────────────────────────────────────────────
provision_pulp(){ # $1=from-apple-xcode-vm  $2=new-vm
  need_tart; local from="${1:-macos-apple-xcode}" vm="${2:-pulp-build-base}"
  note "Tier 2: cloning $from → $vm"
  tart clone "$from" "$vm"
  local rpid; rpid="$(boot_vm "$vm")"
  local ip; ip="$(vm_ip "$vm")"; wait_ssh "$ip"
  vm_ssh "$ip" 'eval "$(/opt/homebrew/bin/brew shellenv)" && brew install --quiet cmake ninja python@3.12 && python3.12 -m pip install --break-system-packages --quiet pillow numpy'
  # Seed prebuilt Skia so the first in-VM configure doesn't rebuild it.
  local repo_root; repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  if [ -d "$repo_root/external/skia-build/build" ]; then
    note "seeding prebuilt Skia into VM (~/pulp-skia-build)"
    vm_ssh "$ip" 'mkdir -p ~/pulp-skia-build'
    vm_rsync "$repo_root/external/skia-build/" "$ip:pulp-skia-build/"
    note "in-VM builds should: export SKIA_DIR=~/pulp-skia-build (FindSkia wants the dir CONTAINING build/, not .../build/mac-gpu)"
  else
    warn "no prebuilt Skia at $repo_root/external/skia-build/build — the VM's first build will fetch/build it"
  fi
  note "Tier 2 provisioned. Pre-warm ccache by running one Release build in-VM before tagging (see headline-path proof in the plan)."
  stop_vm "$vm" "$rpid"
  note "Tier 2 ready. Tag it:  $0 tag $vm pulp-build-base"
}

# ── tag / resize / list ─────────────────────────────────────────────────────
# Tart local VM names are arbitrary strings and may include ':'. We tag golden
# masters as <name>:<date> (CoW clone) so per-job clones come from a frozen,
# dated image and the live VM stays editable.
cmd_tag(){ # $1=src-vm  $2=base-name  [$3=date]
  need_tart; local src="$1" name="$2" d="${3:-$(date +%Y-%m-%d)}"
  [ -n "$src" ] && [ -n "$name" ] || die "usage: tag <src-vm> <base-name> [YYYY-MM-DD]"
  tart clone "$src" "$name:$d"
  note "tagged $src → $name:$d"
  note "also refreshing rolling alias $name:latest"
  tart delete "$name:latest" >/dev/null 2>&1 || true
  tart clone "$name:$d" "$name:latest"
  note "clone for a job:  tart clone $name:latest <job-vm>"
}

# ── Runner layer — CI-complete ephemeral GitHub Actions runner golden ───────
# Clones pulp-build-base and adds the env-parity + agent that let the STOCK
# build.yml workflow pass the full ctest suite in a throwaway VM:
#   • node + git-lfs (threejs smoke + lfs checkout)   — the env gaps behind the
#   • shipyard (pulp status/version/pr CLI tests)        23 in-VM ctest failures
#   • actions-runner agent (configured per-boot via JIT by tools/ci/tart-runner.sh)
# Skia/SDKs/ccache stay handled by the workflow's own setup.sh + actions/cache;
# SKIA_DIR in the runner .env points at the baked Skia so setup.sh can skip it.
RUNNER_VERSION="${PULP_RUNNER_VERSION:-2.334.0}"
HOST_SHIPYARD="${PULP_HOST_SHIPYARD:-$HOME/.local/bin/shipyard}"
provision_runner(){ # $1=from-pulp-vm  $2=new-vm
  need_tart; local from="${1:-pulp-build-base}" vm="${2:-pulp-build-runner}"
  note "Runner layer: cloning $from → $vm"
  tart clone "$from" "$vm"
  local rpid; rpid="$(boot_vm "$vm")"
  local ip; ip="$(vm_ip "$vm")"; wait_ssh "$ip"
  note "installing node + git-lfs (env-parity for threejs smoke + lfs checkout)"
  vm_ssh "$ip" 'eval "$(/opt/homebrew/bin/brew shellenv)" && brew install --quiet node git-lfs && git lfs install'
  if [ -x "$HOST_SHIPYARD" ]; then
    note "copying pinned shipyard ($("$HOST_SHIPYARD" --version 2>/dev/null | head -1)) → VM ~/.local/bin"
    vm_ssh "$ip" 'mkdir -p ~/.local/bin'
    vm_rsync "$HOST_SHIPYARD" "$ip:.local/bin/shipyard"
    vm_ssh "$ip" 'chmod +x ~/.local/bin/shipyard'
  else warn "host shipyard not found at $HOST_SHIPYARD — pulp CLI tests that need it will fail; set PULP_HOST_SHIPYARD"; fi
  note "installing actions-runner agent v$RUNNER_VERSION (configured per-boot via JIT)"
  vm_ssh "$ip" "mkdir -p ~/actions-runner && cd ~/actions-runner && curl -fsSL -o r.tar.gz https://github.com/actions/runner/releases/download/v$RUNNER_VERSION/actions-runner-osx-arm64-$RUNNER_VERSION.tar.gz && tar xzf r.tar.gz && rm r.tar.gz"
  # Runner-wide env: stock workflow picks these up. Baked Skia + ccache warmth +
  # ccache as the compiler launcher (CMake reads these env vars at configure).
  vm_ssh "$ip" 'cat > ~/actions-runner/.env <<ENVV
PATH=/Users/admin/.local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
SKIA_DIR=/Users/admin/pulp-skia-build
PULP_USE_BAKED_SKIA=1
CMAKE_C_COMPILER_LAUNCHER=ccache
CMAKE_CXX_COMPILER_LAUNCHER=ccache
CCACHE_DIR=/Users/admin/Library/Caches/ccache
CCACHE_TEMPDIR=/Users/admin/.ccache-tmp
CCACHE_NOHASHDIR=true
CCACHE_SLOPPINESS=time_macros,pch_defines
ENVV'
  note "Runner layer provisioned."
  stop_vm "$vm" "$rpid"
  note "Runner ready. Tag it:  $0 tag $vm pulp-build-runner   then drive with tools/ci/tart-runner.sh"
}

cmd_resize(){ # $1=vm  $2=GB  — VM must be stopped; guest-agent grows APFS on boot
  need_tart; local vm="$1" gb="$2"
  [ -n "$vm" ] && [ -n "$gb" ] || die "usage: resize <vm> <GB>"
  tart stop "$vm" >/dev/null 2>&1 || true; sleep 2
  tart set "$vm" --disk-size "$gb"
  note "resized $vm disk → ${gb}G. The tart-guest-agent grows the APFS container on next boot."
  note "manual fallback (over ssh, normal boot — NOT recovery): diskutil apfs resizeContainer disk0s2 0"
}

cmd_list(){ need_tart; tart list; echo "--- store (du) ---"; du -sh "$TART_HOME"/vms/* 2>/dev/null || true; }

# ── manifest-driven bake ───────────────────────────────────────────────────
# Reads a per-repo .shipyard/vm-image.toml and bakes (or configures-on-boot) a
# golden image with ZERO hand-provisioning. Same script serves any repo: Pulp
# (Xcode+Skia) and e.g. a light rust profile (no Xcode) both come from one file.
_manifest_read(){ # $1=manifest-path — emits shell KEY=VALUE lines
  python3 - "$1" <<'PY'
import sys, tomllib
m = tomllib.load(open(sys.argv[1], "rb"))
def out(k, v): print(f'{k}={v!r}'.replace("'", '"'))
out("MF_NAME", m.get("name", ""))
out("MF_STRATEGY", m.get("strategy", "bake"))
out("MF_BASE", m.get("base", ""))
out("MF_DISK", str(m.get("disk_gb", 150)))
out("MF_AUTOLOGIN", "1" if m.get("auto_login") else "0")
out("MF_XCODE", m.get("toolchain", {}).get("xcode", ""))
out("MF_BREW", " ".join(m.get("brew", {}).get("packages", [])))
out("MF_PIP", " ".join(m.get("pip", {}).get("packages", [])))
# Intel cross-build layer (optional): rustup + cross targets, and Rosetta 2 so a
# cross-built x86_64 binary can be RUN in-guest. Only manifests that declare
# these pay the cost (Pulp's .shipyard/vm-image.intel.toml).
tc = m.get("toolchain", {})
out("MF_RUST", "1" if tc.get("rust") else "0")
out("MF_RUST_TARGETS", " ".join(tc.get("rust_targets", [])))
out("MF_ROSETTA", "1" if tc.get("rosetta") else "0")
PY
}

cmd_manifest(){ # $1=manifest-path  $2=vm-name(optional)
  need_tart; local path="$1" vm="${2:-}"
  [ -f "$path" ] || die "manifest not found: $path"
  command -v python3 >/dev/null 2>&1 || die "python3 required to parse the manifest"
  local MF_NAME MF_STRATEGY MF_BASE MF_DISK MF_AUTOLOGIN MF_XCODE MF_BREW MF_PIP
  local MF_RUST MF_RUST_TARGETS MF_ROSETTA
  eval "$(_manifest_read "$path")"
  vm="${vm:-$MF_NAME}"
  [ -n "$MF_BASE" ] && [ -n "$vm" ] || die "manifest needs base + name (or pass a vm name)"
  note "manifest: name=$MF_NAME strategy=$MF_STRATEGY base=$MF_BASE disk=${MF_DISK}G xcode=${MF_XCODE:-none}"
  [ -f "$SSH_KEY_PUB" ] || die "ssh pubkey not found: $SSH_KEY_PUB"

  note "cloning $MF_BASE → $vm"
  tart clone "$MF_BASE" "$vm"
  tart set "$vm" --disk-size "$MF_DISK"
  local rpid; rpid="$(boot_vm "$vm")"
  local ip; ip="$(vm_ip "$vm")"; wait_ssh "$ip"
  # Inject key (Tier 0 universal step, identical for every repo).
  vm_ssh "$ip" "mkdir -p ~/.ssh && chmod 700 ~/.ssh && grep -qxF '$(cat "$SSH_KEY_PUB")' ~/.ssh/authorized_keys 2>/dev/null || echo '$(cat "$SSH_KEY_PUB")' >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys"
  vm_ssh "$ip" 'sudo systemsetup -setremotelogin on >/dev/null 2>&1 || true'
  vm_ssh "$ip" 'command -v brew >/dev/null || NONINTERACTIVE=1 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
  [ -n "$MF_BREW" ] && vm_ssh "$ip" "eval \"\$(/opt/homebrew/bin/brew shellenv)\" && brew install --quiet $MF_BREW"
  [ -n "$MF_PIP" ]  && vm_ssh "$ip" "python3 -m pip install --break-system-packages --quiet $MF_PIP"
  # Intel cross-build layer: rustup + cross targets, and Rosetta 2. Only
  # manifests that declare toolchain.rust / .rosetta pay this cost.
  if [ "$MF_RUST" = "1" ]; then
    note "installing Rust toolchain + cross targets: ${MF_RUST_TARGETS:-<none>}"
    vm_ssh "$ip" 'command -v rustup >/dev/null 2>&1 || curl --proto "=https" --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path --default-toolchain stable'
    for _t in $MF_RUST_TARGETS; do
      vm_ssh "$ip" "\$HOME/.cargo/bin/rustup target add $_t"
    done
  fi
  if [ "$MF_ROSETTA" = "1" ]; then
    note "enabling Rosetta 2 (to run cross-built x86_64 binaries in-guest)"
    vm_ssh "$ip" 'arch -x86_64 /usr/bin/true 2>/dev/null || sudo softwareupdate --install-rosetta --agree-to-license 2>/dev/null || true'
  fi
  # Toolchain: only repos that declare toolchain.xcode pay the Xcode cost.
  if [ -n "$MF_XCODE" ]; then
    warn "manifest declares Xcode $MF_XCODE — run '$0 apple-xcode $vm $vm' or set PULP_HOST_XCODE_APP to copy a host Xcode; skipping in manifest bake to keep it unattended"
  fi
  if [ "$MF_STRATEGY" = "configure-on-boot" ]; then
    note "configure-on-boot: $vm is provisioned and left as a reusable base. Clone per-job with tart clone."
    stop_vm "$vm" "$rpid"
  else
    note "bake: provisioning complete."
    stop_vm "$vm" "$rpid"
    note "tag it:  $0 tag $vm $MF_NAME"
  fi
  note "manifest bake done — NO hand-provisioning needed for $MF_NAME"
}

# Preflight the [VERIFY] surface before a long unattended bake.
cmd_verify(){
  need_tart
  note "tart: $(tart --version 2>/dev/null || echo '?')"
  note "TART_HOME=$TART_HOME ($( [ -d "$TART_HOME" ] && echo present || echo MISSING ))"
  [ -f "$SSH_KEY_PUB" ] && note "ssh pubkey: $SSH_KEY_PUB" || warn "ssh pubkey missing: $SSH_KEY_PUB"
  have_sshpass && note "sshpass present (first-boot password auth OK)" || warn "sshpass missing — brew install hudochenkov/sshpass/sshpass (needed for first key injection)"
  if [ -n "$HOST_XCODE_APP" ]; then [ -d "$HOST_XCODE_APP" ] && note "host Xcode to copy: $HOST_XCODE_APP" || warn "PULP_HOST_XCODE_APP set but not found: $HOST_XCODE_APP"
  else warn "PULP_HOST_XCODE_APP unset — Tier 1 will use xcodes (interactive 2FA). Set it to the host's Xcode-${XCODE_VERSION}.0.app to skip re-download."; fi
  note "base image: $BASE_MACOS_IMAGE  (pull check below)"
  tart pull "$BASE_MACOS_IMAGE" --concurrency 4 >/dev/null 2>&1 && note "base image pullable/cached" || warn "could not pull $BASE_MACOS_IMAGE — check name/network"
  note "verify complete"
}

case "${1:-}" in
  verify)       shift; cmd_verify;;
  base)         shift; provision_base "${1:-macos-build-base}";;
  apple-xcode)  shift; provision_apple_xcode "${1:-macos-build-base}" "${2:-macos-apple-xcode}";;
  pulp)         shift; provision_pulp "${1:-macos-apple-xcode}" "${2:-pulp-build-base}";;
  runner)       shift; provision_runner "${1:-pulp-build-base}" "${2:-pulp-build-runner}";;
  tag)          shift; cmd_tag "$@";;
  resize)       shift; cmd_resize "$@";;
  list)         shift; cmd_list;;
  manifest)     shift; cmd_manifest "$@";;
  *) sed -n '2,18p' "$0"; echo; echo "subcommands: verify | base | apple-xcode | pulp | runner | tag | resize | list | manifest <path> [vm]"; exit 1;;
esac
