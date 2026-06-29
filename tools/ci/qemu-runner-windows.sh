#!/usr/bin/env bash
# qemu-runner-windows.sh — ephemeral, per-job GitHub Actions runner on a QEMU
# WINDOWS VM. The Windows analog of tart-runner.sh: mint a JIT (single-job)
# runner config, make a CoW overlay off the Windows golden qcow2 on a dynamic
# free SSH port, boot it, run the agent once, then discard the overlay. Reuses
# the validated boot mechanics from providers/qemu-windows/run.sh.
#
# The runner agent (actions-runner-win-arm64) is installed into C:\actions-runner
# install-if-missing, so this works whether or not the golden has it pre-baked;
# baking it into the golden later just skips the per-job download.
#
# Pilot-safe by default: label `pulp-build-windows` (NOT a required check).
#
# Usage:
#   qemu-runner-windows.sh                 # one ephemeral job then exit (pilot)
#   qemu-runner-windows.sh --loop          # keep serving (LaunchAgent uses this)
#   qemu-runner-windows.sh --labels self-hosted,Windows,ARM64,pulp-build
set -euo pipefail

GOLDEN="${TARTCI_WIN_GOLDEN:-${TARTCI_GOLDENS:-$HOME/VMs/goldens}/pulp-windows-build-24h2-arm64-2026-06-02.qcow2}"
KEY="${TARTCI_WIN_SSH_KEY:-$HOME/.ssh/id_ed25519}"
WUSER="${TARTCI_WIN_SSH_USER:-admin}"
REPO="${PULP_RUNNER_REPO:-danielraffel/pulp}"
LABELS="${PULP_RUNNER_LABELS:-self-hosted,Windows,ARM64,pulp-build-windows}"
RUNNER_GROUP_ID="${PULP_RUNNER_GROUP_ID:-1}"
RUNNER_VERSION="${PULP_RUNNER_VERSION:-2.335.1}"
WORKROOT="${TARTCI_WIN_WORK:-${TMPDIR:-/tmp}/tartci-win}"
LOOP=0; POLL="${PULP_VM_POLL:-20}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 -o IdentitiesOnly=yes -o BatchMode=yes)

note(){ printf '\033[36m• %s\033[0m\n' "$*" >&2; }
die(){ printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }
command -v qemu-system-aarch64 >/dev/null 2>&1 || die "qemu not installed"
command -v gh >/dev/null 2>&1 || die "gh not installed / authed (need admin to mint JIT)"

while [ $# -gt 0 ]; do case "$1" in
  --loop) LOOP=1; shift;;
  --once) LOOP=0; shift;;
  --golden) GOLDEN="$2"; shift 2;;
  --labels) LABELS="$2"; shift 2;;
  --repo) REPO="$2"; shift 2;;
  *) die "unknown arg: $1";;
esac; done

[ -f "$GOLDEN" ] || die "golden not found: $GOLDEN (set TARTCI_WIN_GOLDEN)"
FW=""; for c in /opt/homebrew/share/qemu/edk2-aarch64-code.fd /Applications/UTM.app/Contents/Resources/qemu/edk2-aarch64-code.fd; do [ -f "$c" ] && FW="$c" && break; done
[ -n "$FW" ] || die "no edk2-aarch64-code.fd"
VARS_TPL=""; for v in /opt/homebrew/share/qemu/edk2-aarch64-vars.fd /opt/homebrew/share/qemu/edk2-arm-vars.fd; do [ -f "$v" ] && VARS_TPL="$v" && break; done
[ -n "$VARS_TPL" ] || die "no edk2 vars template"

queued_bat_work(){
  gh api "repos/$REPO/actions/runs?status=queued&per_page=30" \
    --jq '[.workflow_runs[] | select(.name == "Build and Test")] | length' 2>/dev/null || echo 0
}

run_one(){ # $1=iteration index
  local i="$1" jit job="win-ephr-$$-$1"
  note "[$i] minting JIT runner config (labels=$LABELS, ephemeral)"
  local label_args=(); local l; IFS=',' read -ra _ls <<< "$LABELS"
  for l in "${_ls[@]}"; do label_args+=(-f "labels[]=$l"); done
  jit="$(gh api -X POST "repos/$REPO/actions/runners/generate-jitconfig" \
        -f "name=$job" -F "runner_group_id=$RUNNER_GROUP_ID" "${label_args[@]}" \
        --jq '.encoded_jit_config')" || die "JIT mint failed (need repo admin)"
  [ -n "$jit" ] || die "empty JIT config"

  local port jobdir overlay efivars qpid
  port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
  jobdir="$WORKROOT/$job"; mkdir -p "$jobdir"
  overlay="$jobdir/overlay.qcow2"; efivars="$jobdir/efivars.fd"

  note "[$i] CoW overlay off $(basename "$GOLDEN") + boot (ssh 127.0.0.1:$port)"
  qemu-img create -f qcow2 -b "$GOLDEN" -F qcow2 "$overlay" >/dev/null
  cp "$VARS_TPL" "$efivars"
  qemu-system-aarch64 -name "$job" -accel hvf -machine virt,highmem=on -cpu host -smp 8 -m 8192 \
    -drive if=pflash,format=raw,readonly=on,file="$FW" -drive if=pflash,format=raw,file="$efivars" \
    -device ramfb -device qemu-xhci,id=usb -device usb-kbd -device usb-tablet \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$port-:22" -device virtio-net-pci,netdev=net0 \
    -drive file="$overlay",if=none,id=nvm,format=qcow2 -device nvme,drive=nvm,serial=pulpwin \
    -display none >"$jobdir/qemu.log" 2>&1 & qpid=$!

  wsh(){ ssh "${SSH_OPTS[@]}" -i "$KEY" -p "$port" "$WUSER@127.0.0.1" "$@"; }
  # Wait for SSH, but bail the moment QEMU dies — that's how a free-port TOCTOU
  # (another process grabbed $port between the probe close and QEMU's bind)
  # surfaces: qemu exits instantly. Without this check the wait would burn the
  # full ~10min before failing. Caller (--loop) retries with a fresh port.
  local up=0 qemu_died=0; local _
  for _ in $(seq 1 150); do
    kill -0 "$qpid" 2>/dev/null || { qemu_died=1; note "[$i] qemu exited early (well before the SSH window) — port $port likely grabbed (TOCTOU); see $jobdir/qemu.log"; break; }
    wsh 'echo ok' >/dev/null 2>&1 && { up=1; break; }; sleep 4
  done
  if [ "$up" != 1 ]; then
    # qemu-death already logged the accurate cause above; only emit the generic
    # "waited the full window" message when QEMU stayed up but no SSH.
    [ "$qemu_died" = 1 ] || note "[$i] no SSH after ~10min (qemu alive but unreachable; see $jobdir/qemu.log)"
    kill "$qpid" 2>/dev/null || true; rm -rf "$jobdir"; return 1
  fi
  note "[$i] vm $job up — install-if-missing runner + run JIT agent (one job)"

  # The runner agent + JIT run, in three small ssh calls. The JIT blob is
  # multi-KB; it must NEVER ride a command line — embedding it in a PowerShell
  # -EncodedCommand or passing it as a cmd arg blows cmd.exe's 8191-char limit
  # through the ssh→cmd→powershell chain ("The command line is too long").
  # So: (1) install the agent if missing [no blob], (2) STREAM the blob into a
  # file via ssh STDIN [unbounded], (3) run the agent reading that file [no blob].
  local enc_install enc_run
  enc_install="$(printf '%s' '$ProgressPreference="SilentlyContinue"
$dir="C:\actions-runner"
$runnerVersion="'"$RUNNER_VERSION"'"
$listener="$dir\bin\Runner.Listener.exe"
$currentVersion=""
if (Test-Path $listener) {
  try { $currentVersion = ((& $listener --version 2>$null | Select-Object -First 1).Trim()) } catch { $currentVersion = "" }
}
if ($currentVersion -ne $runnerVersion) {
  Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $dir
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $url="https://github.com/actions/runner/releases/download/v$runnerVersion/actions-runner-win-arm64-$runnerVersion.zip"
  Invoke-WebRequest -Uri $url -OutFile "$dir\r.zip"
  Expand-Archive -Path "$dir\r.zip" -DestinationPath $dir -Force
  Remove-Item "$dir\r.zip"
}
# Goldens may carry stale runner registration files from an older proof. JIT
# configs are single-use; leave only the runner binaries before each fresh boot.
Remove-Item -Force -ErrorAction SilentlyContinue "$dir\.runner","$dir\.credentials","$dir\.credentials_rsaparams","$dir\.env","$dir\.path","$dir\jit.cfg"
# Integrity gate: the agent binary must exist after install. The download is
# over authenticated HTTPS and Expand-Archive rejects a corrupt/truncated zip,
# but this catches a partial extract loudly rather than failing opaquely at run.
if (-not (Test-Path "$dir\bin\Runner.Listener.exe")) { Write-Error "Runner.Listener.exe missing after install (corrupt/truncated download?)"; exit 1 }' | iconv -t UTF-16LE | base64)"
  wsh "powershell -NoProfile -EncodedCommand $enc_install" \
    || { note "[$i] runner install failed"; kill "$qpid" 2>/dev/null||true; rm -rf "$jobdir"; return 1; }

  # (2) stream the JIT config in via stdin → file (no command-line length limit).
  # Guard the pipeline: under `set -euo pipefail` a dropped SSH / PowerShell error
  # here would otherwise exit the whole supervisor BEFORE the cleanup below,
  # leaking the QEMU process + overlay for a launchd --loop runner to trip over.
  printf '%s' "$jit" | wsh "powershell -NoProfile -Command \"[Console]::In.ReadToEnd() | Out-File -FilePath C:\\actions-runner\\jit.cfg -Encoding ascii -NoNewline\"" \
    || { note "[$i] JIT config upload failed — discarding overlay"; kill "$qpid" 2>/dev/null||true; rm -rf "$jobdir"; return 1; }

  # (3) run the agent reading the jit FILE — small PS, no blob on the wire
  enc_run="$(printf '%s' 'Set-Location C:\actions-runner
& "C:\actions-runner\bin\Runner.Listener.exe" run --jitconfig (Get-Content "C:\actions-runner\jit.cfg")
exit $LASTEXITCODE' | iconv -t UTF-16LE | base64)"
  wsh "powershell -NoProfile -EncodedCommand $enc_run" \
    || note "[$i] runner exited non-zero (job failure or no job) — overlay discarded regardless"

  note "[$i] discarding ephemeral overlay $job"
  kill "$qpid" 2>/dev/null || true; sleep 1; rm -rf "$jobdir"
}

i=0
if [ "$LOOP" = 1 ]; then
  note "ephemeral Windows runner LOOP (Ctrl-C to stop); golden=$(basename "$GOLDEN") labels=$LABELS"
  while true; do
    q="$(queued_bat_work)"
    if [ "${q:-0}" -gt 0 ]; then
      i=$((i+1)); note "[$i] queued=$q → booting ephemeral Windows VM"; run_one "$i" || true
    else
      note "waiting ${POLL}s (queued=$q — no Build-and-Test work)"; sleep "$POLL"
    fi
  done
else
  note "ephemeral Windows runner ONCE; golden=$(basename "$GOLDEN") labels=$LABELS"
  run_one 1
fi
