#!/usr/bin/env bash
# tart-provision.sh — build/refresh layered "golden master" Tart VMs for CI,
# and resize them. Encodes the tier checklist from
# planning/2026-06-01-macos-ci-isolation-plan.md as reproducible steps (the
# "defined and easy" template), so a new golden master is `provision <tier>`
# rather than hand-clicking.
#
# Images (date-tag with `--tag`):
#   macos-build-base   Tier 0: sshd+key, auto-login, brew(git gh rsync ccache jq
#                              coreutils), tailscale, runner agent          ~50 GB
#   macos-apple-xcode  Tier 1: + Xcode 26.5 (17F42) + CLT, sim runtimes trimmed ~90 GB
#   pulp-build-base    Tier 2: + cmake ninja python@3.12(PIL,numpy) + Skia +
#                              warm ccache/FetchContent                   ~110-130 GB
#
# Storage: TART_HOME=/Volumes/Workshop/VMs (Spotlight-excluded). Clones are CoW.
# NOTE: Tart is not yet installed here — run this in the dedicated CI session
# with Tart present. Steps marked [VERIFY] depend on Tart/Xcode versions; the
# new session should confirm them against `tart --help` + current docs before
# trusting unattended runs. See the plan's "unknowns to validate first".
set -euo pipefail

export TART_HOME="${TART_HOME:-/Volumes/Workshop/VMs}"
SSH_KEY_PUB="${PULP_VM_SSH_KEY_PUB:-$HOME/.ssh/id_ed25519.pub}"
BASE_MACOS_IMAGE="${PULP_MACOS_BASE_IMAGE:-ghcr.io/cirruslabs/macos-sequoia-base:latest}" # [VERIFY] match host macOS / Xcode 26.5 needs Tahoe
XCODE_VERSION="${PULP_XCODE_VERSION:-26.5}"          # build 17F42 — goldens are toolchain-coupled
VM_USER="${PULP_VM_USER:-admin}"; VM_PASS="${PULP_VM_PASS:-admin}"  # Cirrus base default [VERIFY]
DISK_GB="${PULP_VM_DISK_GB:-150}"

note(){ printf '\033[36m• %s\033[0m\n' "$*" >&2; }
die(){ printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }
need_tart(){ command -v tart >/dev/null 2>&1 || die "tart not installed — see plan Phase 3 (brew install cirruslabs/cli/tart)"; }

# Run a command inside a booted VM over ssh (Cirrus base enables ssh w/ user/pass).
vm_ssh(){ local ip="$1"; shift; sshpass -p "$VM_PASS" ssh -o StrictHostKeyChecking=no "$VM_USER@$ip" "$@"; }
vm_ip(){ tart ip "$1"; }

# ── Tier 0 — universal base ────────────────────────────────────────────────
provision_base(){ # $1=vm-name
  need_tart; local vm="$1"
  note "Tier 0: cloning $BASE_MACOS_IMAGE → $vm (disk ${DISK_GB}G)"
  tart clone "$BASE_MACOS_IMAGE" "$vm"
  tart set "$vm" --disk-size "$DISK_GB"            # [VERIFY] grow APFS in-guest if not auto
  tart run --no-graphics "$vm" & local rpid=$!; sleep 30
  local ip; ip="$(vm_ip "$vm")"; note "vm ip=$ip"
  # sshd is on in the Cirrus base; inject our key + the rest.
  [ -f "$SSH_KEY_PUB" ] && vm_ssh "$ip" "mkdir -p ~/.ssh && echo '$(cat "$SSH_KEY_PUB")' >> ~/.ssh/authorized_keys"
  vm_ssh "$ip" 'sudo systemsetup -setremotelogin on || true'      # [VERIFY]
  # auto-login (WindowServer/Metal for GPU tests) — [VERIFY] exact mechanism.
  vm_ssh "$ip" 'echo "enable auto-login per plan Phase 3"'
  # Homebrew + common tooling.
  vm_ssh "$ip" 'command -v brew >/dev/null || /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
  vm_ssh "$ip" 'eval "$(/opt/homebrew/bin/brew shellenv)" && brew install git gh rsync ccache jq coreutils tailscale'
  note "Tier 0 ready in $vm — operator: run 'tailscale up' to authenticate; install the GH runner agent."
  note "Stop + tag:  tart stop $vm  (snapshot/tag externally as macos-build-base:<date>)"
  kill $rpid 2>/dev/null || true
}

# ── Tier 1 — Xcode (Apple projects) ────────────────────────────────────────
provision_apple_xcode(){ # $1=from-base-vm  $2=new-vm
  need_tart; local from="$1" vm="$2"; tart clone "$from" "$vm"
  tart run --no-graphics "$vm" & local rpid=$!; sleep 30
  local ip; ip="$(vm_ip "$vm")"
  vm_ssh "$ip" "eval \"\$(/opt/homebrew/bin/brew shellenv)\" && brew install xcodesorg/made/xcodes && xcodes install $XCODE_VERSION --experimental-unxip"  # [VERIFY] auth/cookies for Xcode download
  vm_ssh "$ip" "sudo xcode-select -s /Applications/Xcode-$XCODE_VERSION.app && sudo xcodebuild -license accept"  # [VERIFY] app name
  vm_ssh "$ip" 'for r in $(xcrun simctl runtime list -j | jq -r "keys[]" 2>/dev/null); do xcrun simctl runtime delete "$r" || true; done'  # trim sims
  note "Tier 1 ready in $vm — tag as macos-apple-xcode:<date>"; kill $rpid 2>/dev/null || true
}

# ── Tier 2 — Pulp ──────────────────────────────────────────────────────────
provision_pulp(){ # $1=from-apple-xcode-vm  $2=new-vm
  need_tart; local from="$1" vm="$2"; tart clone "$from" "$vm"
  tart run --no-graphics "$vm" & local rpid=$!; sleep 30
  local ip; ip="$(vm_ip "$vm")"
  vm_ssh "$ip" 'eval "$(/opt/homebrew/bin/brew shellenv)" && brew install cmake ninja python@3.12 && python3.12 -m pip install --break-system-packages pillow numpy'
  note "Tier 2: copy prebuilt Skia in (rsync external/skia-build) + pre-warm one Release build to seed ccache — see plan."
  note "Tier 2 ready in $vm — tag as pulp-build-base:<date>"; kill $rpid 2>/dev/null || true
}

cmd_resize(){ need_tart; local vm="$1" gb="$2"; tart set "$vm" --disk-size "$gb"; note "resized $vm disk → ${gb}G (grow APFS in-guest if needed: diskutil apfs resizeContainer disk0s2 0)"; }
cmd_list(){ need_tart; tart list; echo "--- store ---"; du -sh "$TART_HOME"/vms/* 2>/dev/null || true; }

case "${1:-}" in
  base)         shift; provision_base "${1:-macos-build-base}";;
  apple-xcode)  shift; provision_apple_xcode "${1:-macos-build-base}" "${2:-macos-apple-xcode}";;
  pulp)         shift; provision_pulp "${1:-macos-apple-xcode}" "${2:-pulp-build-base}";;
  resize)       shift; cmd_resize "$1" "$2";;
  list)         shift; cmd_list;;
  *) sed -n '2,18p' "$0"; exit 1;;
esac
