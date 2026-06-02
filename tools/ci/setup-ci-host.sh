#!/usr/bin/env bash
# setup-ci-host.sh — one-command onboarding of a Mac as a Pulp **Tart VM** CI host.
#
# Opinionated automation of docs/guides/mac-ci-host-setup.md: installs Tart +
# sshpass, sets up TART_HOME (default ~/VMs → no Full Disk Access needed),
# acquires the pulp-build-runner golden (copy from another host, or tells you to
# bake), and installs the persistent ephemeral-runner launchd agent with a
# host-class label. Idempotent. The bare-metal lane is a different tool
# (bootstrap-macos-host.sh); this is the VM-isolated lane.
#
# This setup is OPTIONAL (see the guide). It adds Tart + ~100 GB+ of VM images.
#
# Usage:
#   tools/ci/setup-ci-host.sh --class m5
#   tools/ci/setup-ci-host.sh --class m5 \
#       --copy-from 'macstudio:/Volumes/Workshop/VMs/vms/pulp-build-runner:latest'
#   tools/ci/setup-ci-host.sh --class studio --tart-home /Volumes/Workshop/VMs   # external → prints the FDA step
#   tools/ci/setup-ci-host.sh --class m5 --validate          # also run a one-shot VM build to prove it
#
# Flags:
#   --class <name>     REQUIRED. Host class for the runner label (m5|studio|macbook|…).
#   --copy-from <ssh:path | path>   rsync a golden in from another host/drive (sparse-safe).
#   --tart-home <dir>  VM store (default: $HOME/VMs). On /Volumes → Full Disk Access required.
#   --repo <dir>       Pulp checkout (default: this repo).
#   --no-agent         Do everything except install/load the launchd agent.
#   --validate         After setup, run one ephemeral VM build on the host-only label.
set -euo pipefail

CLASS=""; COPY_FROM=""; TART_HOME_ARG=""; NO_AGENT=0; VALIDATE=0
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RUNNER_VERSION_HINT="2.334.0"

note(){ printf '\033[36m• %s\033[0m\n' "$*" >&2; }
ok(){   printf '\033[32m✓ %s\033[0m\n' "$*" >&2; }
warn(){ printf '\033[33m⚠ %s\033[0m\n' "$*" >&2; }
die(){  printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do case "$1" in
  --class) CLASS="$2"; shift 2;;
  --copy-from) COPY_FROM="$2"; shift 2;;
  --tart-home) TART_HOME_ARG="$2"; shift 2;;
  --repo) REPO_ROOT="$2"; shift 2;;
  --no-agent) NO_AGENT=1; shift;;
  --validate) VALIDATE=1; shift;;
  -h|--help) sed -n '2,30p' "$0"; exit 0;;
  *) die "unknown arg: $1";;
esac; done

[ -n "$CLASS" ] || die "--class <name> is required (e.g. m5, studio, macbook)"
export TART_HOME="${TART_HOME_ARG:-$HOME/VMs}"
LABELS="self-hosted,macos,arm64,pulp-build,pulp-build-${CLASS}"
AGENT="$HOME/Library/LaunchAgents/com.danielraffel.pulp.tart-runner.plist"
TEMPLATE="$REPO_ROOT/tools/launchd/pulp-tart-runner.plist.template"

# ── 1. prereqs ──────────────────────────────────────────────────────────────
note "1/6 prerequisites"
command -v brew >/dev/null || die "Homebrew required: https://brew.sh"
command -v tart >/dev/null || { note "installing tart"; brew install cirruslabs/cli/tart; }
command -v sshpass >/dev/null || { note "installing sshpass (first-boot key inject if baking)"; brew install hudochenkov/sshpass/sshpass || warn "sshpass install failed — only needed if you bake"; }
ok "tart $(tart --version 2>/dev/null)"
[ -f "$HOME/.ssh/id_ed25519" ] || warn "no ~/.ssh/id_ed25519 — the golden must trust your key (ssh-keygen -t ed25519, then re-inject or re-bake)"
gh auth status >/dev/null 2>&1 && ok "gh authenticated" || warn "gh not authenticated — run: gh auth login -h github.com  (needed for the runner agent)"

# ── 2. VM store ─────────────────────────────────────────────────────────────
note "2/6 VM store at $TART_HOME"
mkdir -p "$TART_HOME"; touch "$TART_HOME/.metadata_never_index"
# Pre-create the host ccache dir that the ephemeral runner bind-mounts into each
# VM (`tart run --dir`). Missing on a fresh host → `tart run` fails and the
# runner reports a misleading "no IP" (see the tart-ci skill gotchas).
mkdir -p "${PULP_CI_CACHE:-$HOME/.cache/pulp-ci}/ccache"
grep -q "export TART_HOME=$TART_HOME" "$HOME/.zprofile" 2>/dev/null || echo "export TART_HOME=$TART_HOME" >> "$HOME/.zprofile"
EXTERNAL=0; case "$TART_HOME" in /Volumes/*) EXTERNAL=1;; esac
[ "$EXTERNAL" = 1 ] && warn "TART_HOME is on an external /Volumes drive → the launchd agent needs Full Disk Access (step 5)."

# ── 3. golden image ─────────────────────────────────────────────────────────
note "3/6 pulp-build-runner golden"
if tart list 2>/dev/null | grep -q 'pulp-build-runner:latest'; then
  ok "golden present"
elif [ -n "$COPY_FROM" ]; then
  note "copying golden (sparse-safe) from $COPY_FROM"
  mkdir -p "$TART_HOME/vms"
  rsync -aHS --info=progress2 "$COPY_FROM" "$TART_HOME/vms/" || die "rsync failed (source stopped? path correct?)"
  tart list 2>/dev/null | grep -q 'pulp-build-runner:latest' && ok "golden copied" || die "copied but not visible — check the dir name is exactly 'pulp-build-runner:latest'"
else
  die "no golden + no --copy-from. Either pass --copy-from <host:.../vms/pulp-build-runner:latest>, or bake it: see docs/guides/mac-ci-host-setup.md §1B (tart-provision.sh base→apple-xcode→pulp→runner)."
fi

# ── 4. runner agent template ────────────────────────────────────────────────
[ -f "$TEMPLATE" ] || die "launchd template missing: $TEMPLATE (update your pulp checkout)"

# ── 5. install + load the launchd agent ─────────────────────────────────────
if [ "$NO_AGENT" = 1 ]; then
  note "5/6 --no-agent: skipping launchd install"
else
  note "5/6 installing launchd runner agent (label: pulp-build-${CLASS})"
  mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs/pulp"
  # launchd does not expand $HOME/$PULP_REPO — write absolute paths. Also rewrite
  # TART_HOME + the --labels line for this host.
  sed -e "s|\$PULP_REPO|$REPO_ROOT|g" -e "s|\$HOME|$HOME|g" \
      -e "s|<string>/Volumes/Workshop/VMs</string>|<string>$TART_HOME</string>|g" \
      -e "s|self-hosted,macos,arm64,pulp-build</string>|$LABELS</string>|g" \
      "$TEMPLATE" > "$AGENT"
  if [ "$EXTERNAL" = 1 ]; then
    warn "Full Disk Access REQUIRED before loading: System Settings → Privacy & Security →"
    warn "  Full Disk Access → enable /bin/bash. Then run:  launchctl load $AGENT"
    note "agent written but NOT loaded (external VM store needs FDA first)."
  else
    launchctl unload "$AGENT" 2>/dev/null || true
    launchctl load "$AGENT" && ok "agent loaded (TART_HOME in HOME → no FDA needed)"
    sleep 5; launchctl list | grep -q pulp.tart-runner && ok "agent running" || warn "agent not listed — check $HOME/Library/Logs/pulp/tart-runner.log"
  fi
fi

# ── 6. optional validation ──────────────────────────────────────────────────
if [ "$VALIDATE" = 1 ]; then
  note "6/6 validating: one ephemeral VM build on host-only label pulp-build-${CLASS}"
  note "  start (in another shell): bash $REPO_ROOT/tools/ci/tart-runner.sh --once --labels self-hosted,macos,arm64,pulp-build-${CLASS}"
  note "  then: gh workflow run build.yml --ref main -f macos_runner_selector_json='[\"self-hosted\",\"pulp-build-${CLASS}\"]'"
  note "  (kept manual so you watch it land on THIS host and time it)"
fi

ok "CI host onboarding complete for class '$CLASS'."
note "Pool: this host serves the pulp-build pool via ephemeral VMs. Concurrency cap = 2 VMs/host."
note "Full guide: docs/guides/mac-ci-host-setup.md"
