#!/usr/bin/env bash
# tart-runner.sh — ephemeral, per-job GitHub Actions runner on Tart.
#
# Every CI job runs in a fresh clone of the runner golden, then the VM is
# destroyed. No build-dir churn, no state drift, no ODR class of bug (the
# 0x3f800000 crash). The host stays responsive because builds never touch it
# directly.
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
# CAPACITY+QUEUE-AWARE --loop: the loop boots a VM only when there is
# queued workflow work AND running_macos_vms < cap (PULP_VM_CAP,
# default 2). It cooperates with tools/scripts/macos_reroute_watcher.py, whose
# capacity check is likewise VM-slot-aware ("free VM slot", not
# single-runner busy/idle). Together they close the loop the operator described:
# when the host frees up later, drain still-queued GitHub macOS jobs locally
# instead of leaving them on cloud — without overbooking the 2-VM kernel cap.
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
#   tart-runner.sh --workflow-name Coverage --labels self-hosted,macos,arm64,pulp-coverage-vm-macos
#   tart-runner.sh --golden pulp-build-runner:latest --repo danielraffel/pulp
set -euo pipefail

export TART_HOME="${TART_HOME:-/Volumes/Workshop/VMs}"
SSH_KEY_PRIV="${PULP_VM_SSH_KEY:-$HOME/.ssh/id_ed25519}"
VM_USER="${PULP_VM_USER:-admin}"
CACHE_ROOT="${PULP_CI_CACHE:-$HOME/.cache/pulp-ci}"
GOLDEN="${PULP_RUNNER_GOLDEN:-pulp-build-runner:latest}"
REPO="${PULP_RUNNER_REPO:-danielraffel/pulp}"
LABELS="${PULP_RUNNER_LABELS:-self-hosted,macos,arm64,pulp-build-vm}"
WORKFLOW_NAME="${PULP_RUNNER_WORKFLOW_NAME:-Build and Test}"
MATCH_LABELS="${PULP_RUNNER_QUEUE_MATCH_LABELS:-0}"
RUNNER_GROUP_ID="${PULP_RUNNER_GROUP_ID:-1}"
LOOP=0
CAP="${PULP_VM_CAP:-2}"          # macOS 2-VM kernel cap per host
POLL="${PULP_VM_POLL:-20}"       # seconds to wait when there's no work or no free slot
BACKOFF_MAX="${PULP_VM_BACKOFF_MAX:-300}"  # ceiling for the post-failure backoff
# Static, machine-recognizable runner name (see derive_runner_name below).
# The old `ephr-$$-$i` names changed on every launchd restart and the index grew
# per job, so the same physical Mac showed up under a new throwaway name each
# time. A static name per (host, slot) is the operator's preference and mirrors
# the bare-metal lane
# (bootstrap-macos-host.sh registers pulp-studio-01 with config.sh --replace).
RUNNER_NAME="${PULP_RUNNER_NAME:-}"            # full override; wins if set
RUNNER_NAME_PREFIX="${PULP_RUNNER_NAME_PREFIX:-}"  # else "<prefix>-<slot>"
SLOT="${PULP_RUNNER_SLOT:-1}"                  # distinguishes 2 supervisors on one host (2-VM cap)
PRINT_NAME=0                                   # --print-name: derive + echo the name, then exit (testable, no gh/tart)
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 -o BatchMode=yes)

note(){ printf '\033[36m• %s\033[0m\n' "$*" >&2; }
# `die` is for FATAL PRECONDITIONS only — bad args, missing tart/gh — all of which
# are checked before the supervisor loop starts. It must never be reached from
# inside run_one: `exit` in a function terminates the whole script, and a caller's
# `|| true` cannot catch it, so one transient failure would kill the supervisor.
die(){ printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }
# `soft_fail` is the per-iteration counterpart: same red line, but it RETURNS 1 so
# run_one can unwind and the loop can count the failure, back off, and keep
# serving. Transient failures (JIT mint hiccup, slow boot, network blip) are the
# supervisor's normal weather, not a reason to exit and rely on launchd KeepAlive
# — whose respawn throttle turns a brief outage into a long one.
soft_fail(){ printf '\033[31m✗ %s\033[0m\n' "$*" >&2; return 1; }

while [ $# -gt 0 ]; do case "$1" in
  --loop) LOOP=1; shift;;
  --once) LOOP=0; shift;;
  --golden) GOLDEN="$2"; shift 2;;
  --labels) LABELS="$2"; shift 2;;
  --workflow-name) WORKFLOW_NAME="$2"; shift 2;;
  --queue-match-labels) MATCH_LABELS=1; shift;;
  --repo) REPO="$2"; shift 2;;
  --cap) CAP="$2"; shift 2;;
  --name) RUNNER_NAME="$2"; shift 2;;
  --name-prefix) RUNNER_NAME_PREFIX="$2"; shift 2;;
  --slot) SLOT="$2"; shift 2;;
  --print-name) PRINT_NAME=1; shift;;
  *) die "unknown arg: $1";;
esac; done

# Resolve the runner/VM name once, deterministically (no Date.now/rand — the same
# constraint the old PID+counter scheme satisfied, now satisfied by a stable
# identity instead). Precedence:
#   1. --name / PULP_RUNNER_NAME            — full explicit override.
#   2. "<prefix>-<NN>" where:
#        prefix = --name-prefix / PULP_RUNNER_NAME_PREFIX, else derived from the
#                 host-class label `pulp-build-<class>` → "pulp-<class>", else
#                 "pulp-<short-hostname>".
#        NN     = --slot / PULP_RUNNER_SLOT, zero-padded to 2 digits, so two
#                 supervisors on one host (the 2-VM cap) get -01 / -02.
derive_runner_name(){
  if [ -n "$RUNNER_NAME" ]; then printf '%s' "$RUNNER_NAME"; return; fi
  local prefix="$RUNNER_NAME_PREFIX" l class=""
  if [ -z "$prefix" ]; then
    local _ls; IFS=',' read -ra _ls <<< "$LABELS"
    for l in "${_ls[@]}"; do
      case "$l" in
        pulp-build-vm) ;; # generic pilot label; fall back to host-specific name
        pulp-build-?*) class="${l#pulp-build-}";;
      esac
    done
    if [ -n "$class" ]; then
      prefix="pulp-$class"
    else
      prefix="pulp-$(hostname -s 2>/dev/null | tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9' '-' | sed 's/^-*//;s/-*$//')"
    fi
  fi
  printf '%s-%02d' "$prefix" "$((10#$SLOT))"
}
RUNNER_NAME="$(derive_runner_name)"

if [ "$PRINT_NAME" = 1 ]; then printf '%s\n' "$RUNNER_NAME"; exit 0; fi

command -v tart >/dev/null 2>&1 || die "tart not installed"
command -v gh   >/dev/null 2>&1 || die "gh not installed / authed (need admin to mint JIT config)"

# Count running macOS Tart VMs on THIS host. Linux/Windows guests are uncapped
# (they don't count against the 2-macOS kernel cap), so they're excluded — same
# rule as macos_reroute_watcher.count_running_macos_vms. Any parse/tool failure
# yields 0 so the loop fails safe toward "there may be room" rather than wedging.
running_macos_vms(){
  tart list --format json 2>/dev/null | python3 -c '
import sys, json
try:
    vms = json.load(sys.stdin)
except Exception:
    print(0); raise SystemExit
n = 0
for v in vms if isinstance(vms, list) else []:
    if not str(v.get("State", v.get("state", ""))).lower().startswith("run"):
        continue
    if str(v.get("OS", v.get("os", "darwin"))).lower() in ("", "darwin", "macos"):
        n += 1
print(n)
' 2>/dev/null || echo 0
}

# Tear down the VM this supervisor is currently running, so STOPPING the agent
# reliably reclaims it — RAM, the CoW clone, and the runner registration. This
# fires on `launchctl unload` (the Shipyard GUI "Serve CI builds from this Mac"
# toggle OFF sends SIGTERM) and on Ctrl-C. Without it, a SIGTERM while a VM was
# up (running a job, or a warm JIT runner waiting for one) left a stopped clone
# orphaned on disk and the runner registered-but-offline on GitHub.
CURRENT_VM=""; CURRENT_RPID=""
discard_current_vm(){
  [ -n "$CURRENT_VM" ] || return 0
  note "stopping — tearing down in-flight VM $CURRENT_VM"
  # Be FAST: on `launchctl unload` launchd SIGKILLs this supervisor shortly after
  # SIGTERM, so a graceful `tart stop` + `sleep` can be cut short and leave a
  # stopped clone behind (RAM is freed, but `tart delete` never runs). Hard-kill
  # the `tart run` host process (ends the VM at once), then delete the now-stopped
  # clone immediately — no graceful wait. `tart stop` stays as an idempotent
  # belt-and-suspenders for the rare case rpid is unknown.
  [ -n "$CURRENT_RPID" ] && kill -9 "$CURRENT_RPID" 2>/dev/null || true
  tart stop "$CURRENT_VM" >/dev/null 2>&1 || true
  tart delete "$CURRENT_VM" >/dev/null 2>&1 || true
  CURRENT_VM=""; CURRENT_RPID=""
}
trap 'discard_current_vm; trap - EXIT; exit 143' INT TERM
trap discard_current_vm EXIT

# Coarse "is there macOS work waiting?" gate: count queued runs for the
# configured workflow. Precise cloud-leg targeting is the watcher's job; here
# we only avoid booting a VM when the queue is plainly empty. 0 on any gh
# failure (treat as "no work").
queued_work(){
  if [ "$MATCH_LABELS" != "1" ]; then
    gh api "repos/$REPO/actions/runs?status=queued&per_page=30" \
      --jq "[.workflow_runs[] | select(.name == \"${WORKFLOW_NAME}\")] | length" \
      2>/dev/null || echo 0
    return
  fi

  local label_json count=0 run_id matches
  label_json="$(LABELS="$LABELS" python3 -c 'import json, os; print(json.dumps([x.strip() for x in os.environ["LABELS"].split(",") if x.strip()]))')"
  while IFS= read -r run_id; do
    [ -n "$run_id" ] || continue
    matches="$(
      gh api "repos/$REPO/actions/runs/$run_id/jobs?filter=latest&per_page=100" \
        --jq '.jobs[] | select(.status == "queued") | .labels | @json' 2>/dev/null |
      LABEL_JSON="$label_json" python3 -c '
import json, os, sys
want = {s.lower() for s in json.loads(os.environ["LABEL_JSON"])}
n = 0
for line in sys.stdin:
    try:
        labels = {s.lower() for s in json.loads(line)}
    except Exception:
        continue
    # GitHub assigns a job to a runner iff the runner advertises EVERY label
    # the job requests; the runner may carry extra labels. So this VM, which
    # registers with `want`, can serve a queued job iff the requested job
    # labels are a subset of `want`. The reverse direction (want as a subset
    # of the job labels) silently fails to boot when the runner carries an
    # extra label such as a per-host pulp-build-vm-secondary.
    if labels.issubset(want):
        n += 1
print(n)
'
    )" || matches=0
    count=$((count + ${matches:-0}))
  done < <(
    # Scan BOTH queued and in_progress runs: a workflow with an early
    # GitHub-hosted resolver/classify job (Coverage, Release CLI) flips to
    # `in_progress` before its self-hosted leg is even queued, so a
    # queued-only run scan would never see the self-hosted job and the VM
    # would never boot. Matches the tartci macOS provider's loop.
    for st in queued in_progress; do
      gh api "repos/$REPO/actions/runs?status=$st&per_page=30" \
        --jq ".workflow_runs[] | select(.name == \"${WORKFLOW_NAME}\") | .id" 2>/dev/null || true
    done
  )
  echo "$count"
}

# Reclaim a static runner name before reusing it: a JIT runner that finished
# normally self-removes, but a SIGKILL'd supervisor, an errored job, or a crashed
# clone can leave (a) a stale GitHub registration (shows "Offline") and/or (b) a
# stopped Tart clone of the same name. Either would block reuse — `generate-jitconfig`
# rejects a duplicate name and `tart clone` rejects an existing VM. This is the
# JIT-lane equivalent of bare-metal `config.sh --replace`. Best-effort; never fatal.
reclaim_runner_name(){ # $1=name
  local name="$1" id
  id="$(gh api "repos/$REPO/actions/runners" --paginate \
        --jq ".runners[] | select(.name==\"$name\") | .id" 2>/dev/null | head -n1 || true)"
  if [ -n "$id" ]; then
    note "reclaiming static name '$name': deleting stale runner registration (id=$id)"
    gh api -X DELETE "repos/$REPO/actions/runners/$id" >/dev/null 2>&1 || true
  fi
  tart delete "$name" >/dev/null 2>&1 || true   # clear a crashed leftover clone of the same name
}

run_one(){ # $1=iteration index — log label only; the VM/runner name is now static per (host, slot)
  local i="$1" vm="$RUNNER_NAME" jit
  reclaim_runner_name "$vm"
  note "[$i] minting JIT runner config (name=$vm, labels=$LABELS, ephemeral)"
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
  CURRENT_VM="$vm"   # from here on, a stop/SIGTERM must tear this VM down
  # The --dir mount target MUST exist on the host. If it doesn't, `tart run`
  # exits instantly with "directory sharing device configuration is invalid"
  # (VZErrorDomain Code=2) and the VM never boots — which then surfaces only as
  # a misleading "no IP" below. Create it so a fresh host can't trip on it.
  mkdir -p "$CACHE_ROOT/ccache"
  local boot_log; boot_log="$(mktemp -t "tart-run-$vm")"
  local rpid; tart run --no-graphics --dir="ccache:$CACHE_ROOT/ccache" "$vm" >"$boot_log" 2>&1 & rpid=$!
  CURRENT_RPID="$rpid"

  local ip=""; local k
  for k in $(seq 1 60); do ip="$(tart ip "$vm" 2>/dev/null || true)"; [ -n "$ip" ] && break; sleep 2; done
  if [ -z "$ip" ]; then
    # Surface the real reason instead of a bare "no IP": the boot log usually
    # names it (bad --dir mount, vmnet/Local-Network permission, slow boot).
    note "[$i] no IP after 120s — last lines of \`tart run\` ($boot_log):"; tail -3 "$boot_log" >&2 2>/dev/null || true
    tart stop "$vm" >/dev/null 2>&1||true; kill "$rpid" 2>/dev/null||true; tart delete "$vm" >/dev/null 2>&1||true
    CURRENT_VM=""; CURRENT_RPID=""
    rm -f "$boot_log"
    soft_fail "[$i] no IP (see \`tart run\` output above)"; return 1
  fi
  rm -f "$boot_log"
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
  CURRENT_VM=""; CURRENT_RPID=""
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
  note "ephemeral runner LOOP (Ctrl-C to stop); workflow=$WORKFLOW_NAME labels_match=$MATCH_LABELS golden=$GOLDEN labels=$LABELS cap=$CAP"
  while true; do
    q="$(queued_work)"; r="$(running_macos_vms)"
    if [ "${q:-0}" -gt 0 ] && [ "${r:-0}" -lt "$CAP" ]; then
      i=$((i+1)); note "[$i] queued=$q running_vms=$r/$CAP → booting ephemeral VM"
      if run_one "$i"; then
        fails=0
      else
        # run_one cleans up its own VM on every failure path it owns; this is the
        # belt-and-suspenders for one it doesn't (a no-op when CURRENT_VM is
        # empty), so a failed iteration can never leak a clone into the next one.
        discard_current_vm
        fails=$((fails + 1))
        b="$(backoff_secs "$fails")"
        note "[$i] iteration failed (consecutive=$fails) — backing off ${b}s, then continuing"
        sleep "$b"
      fi
    else
      note "waiting ${POLL}s (queued=$q running_vms=$r/$CAP — no work or no free slot)"
      sleep "$POLL"
    fi
  done
else
  note "ephemeral runner ONCE; workflow=$WORKFLOW_NAME golden=$GOLDEN labels=$LABELS"
  run_one 1
fi
