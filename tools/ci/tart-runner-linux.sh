#!/usr/bin/env bash
# tart-runner-linux.sh — ephemeral, per-job GitHub Actions runner on a Tart LINUX
# VM. The Linux analog of tart-runner.sh: mint a Just-In-Time (single-job) runner
# config, clone the pulp-linux-build golden, boot it with the host ccache mounted,
# run the agent once with that JIT config, then discard the clone. Native arm64
# Ubuntu 24.04; Skia is baked in-checkout (FindSkia auto-discovers it).
#
# The golden must carry the actions-runner agent at ~/actions-runner (baked by
# tart-provision.sh's runner step / the linux-arm64 install). This supervisor
# never registers a long-lived runner — JIT configs are single-job and ephemeral.
#
# CONCURRENCY: Linux guests are UNCAPPED (no 2-VM macOS kernel quota), so this can
# run several concurrent clones on one host; the --loop gate still only boots when
# there is queued Build-and-Test work, to avoid spinning idle VMs.
#
# Pilot-safe by default: the label is `pulp-build-linux` (NOT a required check),
# so jobs only land here when a workflow explicitly routes to it. Promote to the
# pooled label once a pilot is clean.
#
# Usage:
#   tart-runner-linux.sh                 # one ephemeral job then exit (pilot default)
#   tart-runner-linux.sh --loop          # keep serving jobs (LaunchAgent uses this)
#   tart-runner-linux.sh --labels self-hosted,Linux,ARM64,pulp-build
set -euo pipefail

export TART_HOME="${TART_HOME:-/Volumes/Workshop/VMs}"
SSH_KEY_PRIV="${PULP_VM_SSH_KEY:-$HOME/.ssh/id_ed25519}"
VM_USER="${PULP_VM_USER:-admin}"
CACHE_ROOT="${PULP_CI_CACHE:-$HOME/.cache/pulp-ci}"
GOLDEN="${PULP_LINUX_GOLDEN:-pulp-linux-build:latest}"
REPO="${PULP_RUNNER_REPO:-danielraffel/pulp}"
LABELS="${PULP_RUNNER_LABELS:-self-hosted,Linux,ARM64,pulp-build-linux}"
RUNNER_GROUP_ID="${PULP_RUNNER_GROUP_ID:-1}"
LOOP=0
POLL="${PULP_VM_POLL:-20}"
BACKOFF_MAX="${PULP_VM_BACKOFF_MAX:-300}"  # ceiling for the post-failure backoff
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 -o BatchMode=yes)

note(){ printf '\033[36m• %s\033[0m\n' "$*" >&2; }
# `die` is for FATAL PRECONDITIONS only (bad args, missing tart/gh), all checked
# before the loop starts. Never reach it from run_one: `exit` in a function kills
# the whole script and a caller's `|| true` cannot catch it, so one transient
# failure would take the supervisor down with it. Use `soft_fail` there instead —
# it returns 1 so the loop can count, back off, and keep serving.
die(){ printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }
soft_fail(){ printf '\033[31m✗ %s\033[0m\n' "$*" >&2; return 1; }
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

# Coarse "is there work waiting?" gate for --loop: count queued Build-and-Test
# runs. 0 on any gh failure (treat as "no work" so a flaky API doesn't spin VMs).
queued_bat_work(){
  gh api "repos/$REPO/actions/runs?status=queued&per_page=30" \
    --jq '[.workflow_runs[] | select(.name == "Build and Test")] | length' \
    2>/dev/null || echo 0
}

run_one(){ # $1=iteration index (unique VM name without Date.now/rand)
  local i="$1" vm="linux-ephr-$$-$1" jit
  note "[$i] minting JIT runner config (labels=$LABELS, ephemeral)"
  local label_args=(); local l; IFS=',' read -ra _ls <<< "$LABELS"
  for l in "${_ls[@]}"; do label_args+=(-f "labels[]=$l"); done
  jit="$(gh api -X POST "repos/$REPO/actions/runners/generate-jitconfig" \
        -f "name=$vm" -F "runner_group_id=$RUNNER_GROUP_ID" "${label_args[@]}" \
        --jq '.encoded_jit_config')" \
    || { soft_fail "[$i] JIT config mint failed (transient API error, or gh lacks repo admin)"; return 1; }
  [ -n "$jit" ] || { soft_fail "[$i] empty JIT config"; return 1; }

  note "[$i] clone $GOLDEN → $vm (CoW) + boot with host ccache mounted"
  # Explicit check, not errexit: run_one is invoked from the loop in a context
  # that suppresses `set -e` for its whole body, so an unchecked failure here
  # would fall through and boot nothing.
  tart clone "$GOLDEN" "$vm" || { soft_fail "[$i] tart clone $GOLDEN → $vm failed"; return 1; }
  mkdir -p "$CACHE_ROOT/ccache-linux"
  local boot_log; boot_log="$(mktemp -t "tart-run-$vm")"
  local rpid; tart run --no-graphics --dir="ccache:$CACHE_ROOT/ccache-linux" "$vm" >"$boot_log" 2>&1 & rpid=$!

  local ip=""
  for _ in $(seq 1 60); do ip="$(tart ip "$vm" 2>/dev/null || true)"; [ -n "$ip" ] && break; sleep 2; done
  if [ -z "$ip" ]; then
    note "[$i] no IP after 120s — last lines of \`tart run\` ($boot_log):"; tail -3 "$boot_log" >&2 2>/dev/null || true
    tart stop "$vm" >/dev/null 2>&1||true; kill "$rpid" 2>/dev/null||true; tart delete "$vm" >/dev/null 2>&1||true
    rm -f "$boot_log"
    soft_fail "[$i] no IP (see \`tart run\` output above)"; return 1
  fi
  rm -f "$boot_log"
  local sshok=0
  for _ in $(seq 1 90); do ssh "${SSH_OPTS[@]}" -i "$SSH_KEY_PRIV" "$VM_USER@$ip" true 2>/dev/null && { sshok=1; break; }; sleep 2; done
  if [ "$sshok" != 1 ]; then
    note "[$i] no SSH on $vm after 180s — discarding (won't run a job on an unreachable VM)"
    tart stop "$vm" >/dev/null 2>&1 || true; kill "$rpid" 2>/dev/null || true; tart delete "$vm" >/dev/null 2>&1 || true
    return 1
  fi
  note "[$i] vm $vm up at $ip — mounting ccache + launching JIT runner (one job)"

  # Best-effort host ccache via virtio-fs (named "ccache" subdir is the rw one).
  # Then write the JIT config and run the agent once — a JIT runner processes
  # exactly one job and deregisters. No brew (Linux); CCACHE_* come from the
  # baked ~/actions-runner/.env so job steps inherit warm-cache settings.
  ssh "${SSH_OPTS[@]}" -i "$SSH_KEY_PRIV" "$VM_USER@$ip" \
    "sudo mkdir -p /mnt/host 2>/dev/null; sudo mount -t virtiofs com.apple.virtio-fs.automount /mnt/host 2>/dev/null || true; \
     if [ -d /mnt/host/ccache ] && [ -w /mnt/host/ccache ]; then mkdir -p ~/.ccache && ln -sfn /mnt/host/ccache ~/.ccache; fi; \
     printf '%s' '$jit' > ~/jit.cfg && cd ~/actions-runner && ./run.sh --jitconfig \"\$(cat ~/jit.cfg)\"" \
    || note "[$i] runner exited non-zero (job failure or no job) — VM will be discarded regardless"

  note "[$i] discarding ephemeral VM $vm"
  tart stop "$vm" >/dev/null 2>&1 || true; kill "$rpid" 2>/dev/null || true; sleep 2
  tart delete "$vm" >/dev/null 2>&1 || true
}

# Exponential backoff for CONSECUTIVE failed iterations, capped at BACKOFF_MAX:
# POLL, 2*POLL, 4*POLL, … A tight retry against a rate-limited API or a wedged
# host is its own outage, but a supervisor that stops retrying is a worse one.
backoff_secs(){ # $1=consecutive failure count (>=1)
  local n="$1" s="$POLL"
  while [ "$n" -gt 1 ]; do
    s=$((s * 2)); n=$((n - 1))
    [ "$s" -ge "$BACKOFF_MAX" ] && { s="$BACKOFF_MAX"; break; }
  done
  printf '%s' "$s"
}

i=0
fails=0
if [ "$LOOP" = 1 ]; then
  note "ephemeral Linux runner LOOP (Ctrl-C to stop); golden=$GOLDEN labels=$LABELS"
  while true; do
    q="$(queued_bat_work)"
    if [ "${q:-0}" -gt 0 ]; then
      i=$((i+1)); note "[$i] queued=$q → booting ephemeral Linux VM"
      if run_one "$i"; then
        fails=0
      else
        fails=$((fails + 1))
        b="$(backoff_secs "$fails")"
        note "[$i] iteration failed (consecutive=$fails) — backing off ${b}s, then continuing"
        sleep "$b"
      fi
    else
      note "waiting ${POLL}s (queued=$q — no Build-and-Test work)"; sleep "$POLL"
    fi
  done
else
  note "ephemeral Linux runner ONCE; golden=$GOLDEN labels=$LABELS"
  run_one 1
fi
