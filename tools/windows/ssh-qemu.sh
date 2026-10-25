#!/usr/bin/env bash
set -euo pipefail

# Windows OpenSSH in the disposable guest must never inherit the caller's
# stdin. Commands that read stdin can otherwise hold the guest channel open and
# make a healthy QEMU session look wedged. Avoid the ssh-agent's rotating key
# set as well; the guest image authorizes this explicit file-backed key.
# IdentitiesOnly alone still lets 1Password sign for that key and prompt.
port="${SPECTR_WINDOWS_SSH_PORT:-50375}"
key="${SPECTR_WINDOWS_SSH_KEY:-}"
if [[ -z "$key" ]]; then
  # The native ARM guest is provisioned from the M5/M5S host key. Keep the
  # generic fallback for fresh images and explicit operator overrides.
  if [[ -f "$HOME/.ssh/id_ed25519_macstudio" ]]; then
    key="$HOME/.ssh/id_ed25519_macstudio"
  else
    key="$HOME/.ssh/id_ed25519"
  fi
fi
[[ -f "$key" ]] || { echo "missing SSH key: $key" >&2; exit 1; }
exec ssh -n -T -o BatchMode=yes -o IdentitiesOnly=yes -o IdentityAgent=none -i "$key" \
  -o ConnectTimeout="${SPECTR_WINDOWS_SSH_TIMEOUT:-5}" \
  -o ConnectionAttempts="${SPECTR_WINDOWS_SSH_CONNECTION_ATTEMPTS:-3}" \
  -o ControlMaster=no -o ControlPath=none \
  -o ServerAliveInterval="${SPECTR_WINDOWS_SSH_KEEPALIVE_INTERVAL:-5}" \
  -o ServerAliveCountMax="${SPECTR_WINDOWS_SSH_KEEPALIVE_COUNT:-2}" \
  -o StrictHostKeyChecking=no -p "$port" admin@127.0.0.1 "$@"
