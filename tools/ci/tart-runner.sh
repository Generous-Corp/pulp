#!/usr/bin/env bash
# tart-runner.sh — ephemeral, per-job GitHub Actions runner on Tart.
#
# This is the "right long-term" execution model from
# planning/2026-06-01-macos-ci-isolation-plan.md: every CI job runs in a FRESH
# clone of the runner golden, then the VM is destroyed. No build-dir churn, no
# state drift, no ODR class of bug (the 0x3f800000 crash) — ever. The host stays
# responsive because builds never touch it directly.
#
# Mechanism: mint a Just-In-Time (JIT) runner config from GitHub (inherently
# single-job / ephemeral), clone pulp-build-runner, boot it with the host ccache
# mounted for warmth, run the agent once with that JIT config, then discard.
#
# CONCURRENCY: macOS caps 2 running VMs PER HOST (kernel quota; see plan
# Appendix D). With `pulp-vm` (operator's box) running, only 1 ephemeral slot is
# free on that host. To exceed 2 concurrent, run this supervisor on multiple
# Macs (the same hosts that run the bare-metal runners today) or enable the
# Appendix-D quota override on the dedicated Studio.
# TODO (capacity+queue-aware --loop): only boot a VM when there is queued work
# AND running_macos_vms < cap, cooperating with tools/scripts/macos_reroute_watcher.py
# (task #22), whose "local idle" check should evolve to "free VM slot". That
# closes the loop the operator described: when the host frees up later, drain
# still-queued GitHub macOS jobs locally instead of leaving them on cloud.
#
# Pilot-safe by default: the label is `pulp-build-vm` (NOT the required
# `pulp-build`), so jobs only land here when explicitly routed
# (`gh workflow run build.yml -f macos_runner_selector_json='["self-hosted","pulp-build-vm"]'`).
# Promote to `pulp-build` only after a clean pilot.
#
# Usage:
#   tart-runner.sh                     # one ephemeral job then exit (pilot default)
#   tart-runner.sh --loop              # keep spinning a fresh VM after each job
#   tart-runner.sh --labels self-hosted,macos,arm64,pulp-build   # promote
#   tart-runner.sh --golden pulp-build-runner:latest --repo danielraffel/pulp
set -euo pipefail

export TART_HOME="${TART_HOME:-/Volumes/Workshop/VMs}"
SSH_KEY_PRIV="${PULP_VM_SSH_KEY:-$HOME/.ssh/id_ed25519}"
VM_USER="${PULP_VM_USER:-admin}"
CACHE_ROOT="${PULP_CI_CACHE:-$HOME/.cache/pulp-ci}"
GOLDEN="${PULP_RUNNER_GOLDEN:-pulp-build-runner:latest}"
REPO="${PULP_RUNNER_REPO:-danielraffel/pulp}"
LABELS="${PULP_RUNNER_LABELS:-self-hosted,macos,arm64,pulp-build-vm}"
RUNNER_GROUP_ID="${PULP_RUNNER_GROUP_ID:-1}"
LOOP=0
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 -o BatchMode=yes)

note(){ printf '\033[36m• %s\033[0m\n' "$*" >&2; }
die(){ printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }
command -v tart >/dev/null 2>&1 || die "tart not installed"
command -v gh   >/dev/null 2>&1 || die "gh not installed / authed (need admin to mint JIT config)"

while [ $# -gt 0 ]; do case "$1" in
  --loop) LOOP=1; shift;;
  --once) LOOP=0; shift;;
  --golden) GOLDEN="$2"; shift 2;;
  --labels) LABELS="$2"; shift 2;;
  --repo) REPO="$2"; shift 2;;
  *) die "unknown arg: $1";;
esac; done

run_one(){ # $1=iteration index (keeps VM name unique without Date.now/rand)
  local i="$1" vm="ephr-$$-$1" jit
  note "[$i] minting JIT runner config (labels=$LABELS, ephemeral)"
  local label_args=(); local l; IFS=',' read -ra _ls <<< "$LABELS"
  for l in "${_ls[@]}"; do label_args+=(-f "labels[]=$l"); done
  jit="$(gh api -X POST "repos/$REPO/actions/runners/generate-jitconfig" \
        -f "name=$vm" -F "runner_group_id=$RUNNER_GROUP_ID" "${label_args[@]}" \
        --jq '.encoded_jit_config')" || die "JIT config mint failed (need repo admin)"
  [ -n "$jit" ] || die "empty JIT config"

  note "[$i] clone $GOLDEN → $vm (CoW) + boot with host ccache mounted"
  tart clone "$GOLDEN" "$vm"
  local rpid; tart run --no-graphics --dir="ccache:$CACHE_ROOT/ccache" "$vm" >/dev/null 2>&1 & rpid=$!

  local ip=""; local k
  for k in $(seq 1 60); do ip="$(tart ip "$vm" 2>/dev/null || true)"; [ -n "$ip" ] && break; sleep 2; done
  [ -n "$ip" ] || { tart stop "$vm" >/dev/null 2>&1||true; kill "$rpid" 2>/dev/null||true; tart delete "$vm" >/dev/null 2>&1||true; die "[$i] no IP"; }
  for k in $(seq 1 90); do ssh "${SSH_OPTS[@]}" -i "$SSH_KEY_PRIV" "$VM_USER@$ip" true 2>/dev/null && break; sleep 2; done
  note "[$i] vm $vm up at $ip — wiring ccache mount + launching JIT runner (one job)"

  # Point the (no-space) CCACHE_DIR from the golden .env at the virtio-fs mount,
  # keep CCACHE_TEMPDIR in-guest (set in the golden .env). Then run the agent
  # once; a JIT runner processes exactly one job and deregisters.
  # Source brew's shellenv so the runner PROCESS (and therefore every job step
  # it spawns) inherits /opt/homebrew/bin on PATH + HOMEBREW_*. Without this the
  # SSH non-interactive shell has a minimal PATH and `brew` is exit-127 in steps
  # (the bare-metal runners get this via their login shell instead).
  ssh "${SSH_OPTS[@]}" -i "$SSH_KEY_PRIV" "$VM_USER@$ip" \
    "mkdir -p ~/Library/Caches ~/.ccache-tmp && ln -sfn '/Volumes/My Shared Files/ccache' ~/Library/Caches/ccache && \
     printf '%s' '$jit' > ~/jit.cfg && eval \"\$(/opt/homebrew/bin/brew shellenv)\" && cd ~/actions-runner && ./run.sh --jitconfig \"\$(cat ~/jit.cfg)\"" \
    || note "[$i] runner exited non-zero (job failure or no job) — VM will be discarded regardless"

  note "[$i] discarding ephemeral VM $vm"
  tart stop "$vm" >/dev/null 2>&1 || true; kill "$rpid" 2>/dev/null || true; sleep 2
  tart delete "$vm" >/dev/null 2>&1 || true
}

i=0
if [ "$LOOP" = 1 ]; then
  note "ephemeral runner LOOP (Ctrl-C to stop); golden=$GOLDEN labels=$LABELS"
  while true; do i=$((i+1)); run_one "$i" || true; done
else
  note "ephemeral runner ONCE; golden=$GOLDEN labels=$LABELS"
  run_one 1
fi
