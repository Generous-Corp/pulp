#!/usr/bin/env bash
#
# Move a release-cli build leg between GitHub-hosted runners, the local VM pool,
# or one specific machine — without touching a line of code.
#
#   tools/scripts/release_routing.sh show
#   tools/scripts/release_routing.sh local  linux-arm64
#   tools/scripts/release_routing.sh pin    linux-arm64 m5      # one machine
#   tools/scripts/release_routing.sh github linux-arm64         # revert
#
# Routing is a repo variable per leg, read by release-cli.yml's resolver
# (tools/scripts/resolve_release_runners.py). UNSET means GitHub-hosted, so
# `github <leg>` is a complete revert and takes effect on the very next tag.
#
# WHY per-leg + per-machine: a slow SDK build that is hogging a box you need for
# something else should be movable in one command, not a PR.
#
# NOTE the two x64 legs deliberately have no local preset. Your Linux and Windows
# VMs are ARM64 guests, so an x86_64 build there means emulation. These are SHIPPED
# artifacts — a subtly-wrong emulated binary is worse than a slow correct one, and
# the hosted x64 queues are near-zero anyway. You can still force it by passing an
# explicit labelset, but you have to mean it.
set -euo pipefail

REPO="${PULP_REPO:-Generous-Corp/pulp}"
GH="${TARTCI_GH_CLI:-ghapp}"
command -v "$GH" >/dev/null 2>&1 || GH=gh

declare -A VAR=(
  [darwin-arm64]=PULP_RELEASE_DARWIN_ARM64_RUNS_ON_JSON
  [darwin-x64]=PULP_RELEASE_DARWIN_X64_RUNS_ON_JSON
  [linux-x64]=PULP_RELEASE_LINUX_X64_RUNS_ON_JSON
  [linux-arm64]=PULP_RELEASE_LINUX_ARM64_RUNS_ON_JSON
  [windows-x64]=PULP_RELEASE_WINDOWS_X64_RUNS_ON_JSON
  [windows-arm64]=PULP_RELEASE_WINDOWS_ARM64_RUNS_ON_JSON
)

# The local pool labelset per leg. Empty = no local preset (see NOTE above).
declare -A LOCAL=(
  [darwin-arm64]='["self-hosted","macOS","ARM64","pulp-build-vm-release"]'
  [darwin-x64]='["self-hosted","macOS","ARM64","pulp-build-vm-release"]'
  [linux-arm64]='["self-hosted","Linux","ARM64","pulp-build-linux"]'
  [windows-arm64]='["self-hosted","Windows","ARM64","pulp-build-windows"]'
  [linux-x64]=''
  [windows-x64]=''
)

# Host labels, for pinning a leg to one machine.
declare -A HOST=(
  [macstudio]=pulp-host-macstudio
  [m5]=pulp-host-m5
  [m1]=pulp-host-m1
)

die() { echo "error: $*" >&2; exit 1; }
known_leg() { [[ -n "${VAR[$1]:-}" ]] || die "unknown leg '$1' (legs: ${!VAR[*]})"; }

case "${1:-show}" in
  show)
    printf '%-14s %-9s %s\n' LEG WHERE 'runs-on'
    for leg in darwin-arm64 darwin-x64 linux-arm64 linux-x64 windows-arm64 windows-x64; do
      val="$("$GH" variable get "${VAR[$leg]}" --repo "$REPO" 2>/dev/null || true)"
      if [[ -n "$val" ]]; then printf '%-14s %-9s %s\n' "$leg" LOCAL "$val"
      else                     printf '%-14s %-9s %s\n' "$leg" github '(default)'; fi
    done
    ;;

  local)
    leg="${2:?usage: local <leg>}"; known_leg "$leg"
    labels="${LOCAL[$leg]}"
    [[ -n "$labels" ]] || die "$leg has no local preset: your Linux/Windows VMs are ARM64 guests, so an x64 build there is emulated. Pass an explicit labelset if you really mean it."
    "$GH" variable set "${VAR[$leg]}" --repo "$REPO" --body "$labels"
    echo "$leg -> LOCAL pool: $labels"
    ;;

  pin)
    leg="${2:?usage: pin <leg> <machine>}"; machine="${3:?usage: pin <leg> <machine>}"
    known_leg "$leg"
    host="${HOST[$machine]:-}"; [[ -n "$host" ]] || die "unknown machine '$machine' (${!HOST[*]})"
    base="${LOCAL[$leg]}"; [[ -n "$base" ]] || die "$leg has no local preset (see NOTE in this script)"
    labels="$(python3 -c "import json,sys; a=json.loads(sys.argv[1]); a.append(sys.argv[2]); print(json.dumps(a))" "$base" "$host")"
    "$GH" variable set "${VAR[$leg]}" --repo "$REPO" --body "$labels"
    echo "$leg -> PINNED to $machine: $labels"
    ;;

  github)
    leg="${2:?usage: github <leg>}"; known_leg "$leg"
    "$GH" variable delete "${VAR[$leg]}" --repo "$REPO" 2>/dev/null || true
    echo "$leg -> GitHub-hosted (variable unset; effective on the next tag)"
    ;;

  *) sed -n '2,20p' "$0"; exit 1;;
esac
