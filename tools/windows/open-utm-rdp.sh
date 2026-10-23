#!/usr/bin/env bash
set -euo pipefail

# Open the retained ARM64 Windows guest in a visible native RDP window. UTM's
# emulated framebuffer is not a usable Windows display for this image, so RDP
# is the supported interactive transport. This does not reserve a TartCI
# runner and does not create another disk image.
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
utmctl="/Applications/UTM.app/Contents/MacOS/utmctl"
config_file="${SPECTR_WINDOWS_UTM_CONFIG:-$HOME/.config/pulp/windows-utm.env}"
if [[ -f "$config_file" ]]; then
  # This file contains only machine-local UUID/forwarding values. Credentials
  # are read separately below and are never sourced from this file.
  # shellcheck disable=SC1090
  source "$config_file"
fi
utm_uuid="${SPECTR_WINDOWS_UTM_UUID:-}"
ssh_port="${SPECTR_WINDOWS_SSH_PORT:-}"
rdp_port="${SPECTR_WINDOWS_RDP_PORT:-}"
rdp_domain="${SPECTR_WINDOWS_RDP_DOMAIN:-PULP-WIN}"
secret_file="${SPECTR_WINDOWS_UTM_SECRET:-$HOME/.config/pulp/secrets/spectr-windows-utm-admin}"

for required in utm_uuid ssh_port rdp_port; do
  [[ -n "${!required}" ]] || {
  echo "set guest values in $config_file or SPECTR_WINDOWS_UTM_UUID, SPECTR_WINDOWS_SSH_PORT, and SPECTR_WINDOWS_RDP_PORT" >&2
    exit 2
  }
done

[[ -x "$utmctl" ]] || { echo "UTM is not installed at $utmctl" >&2; exit 1; }
[[ -f "$secret_file" ]] || { echo "missing credential file: $secret_file" >&2; exit 2; }
if [[ "$(stat -f %Lp "$secret_file")" != 600 ||
      "$(stat -f %Lp "$(dirname "$secret_file")")" != 700 ]]; then
  echo "credential file must be 0600 inside a 0700 directory" >&2
  exit 2
fi
command -v sdl-freerdp >/dev/null || { echo "sdl-freerdp is required" >&2; exit 2; }
unset username password
# shellcheck disable=SC1090
source "$secret_file"
: "${username:?credential file must define username}"
: "${password:?credential file must define password}"
export -n username password

status="$($utmctl list | awk -v uuid="$utm_uuid" '$1 == uuid { print $2; exit }')"
[[ -n "$status" ]] || {
  echo "configured UTM UUID is absent; inspect '$utmctl list' and verify the guest before retrying" >&2
  exit 2
}
if [[ "$status" != "started" ]]; then
  "$utmctl" start "$utm_uuid" >/dev/null
fi

# Windows can open its forwarded TCP port before OpenSSH is ready after boot.
# Wait for this guest: another QEMU guest on 50375 may have different credentials.
echo 'Waiting for Windows sign-in services...'
deadline=$((SECONDS + ${SPECTR_WINDOWS_READY_TIMEOUT:-120}))
until SPECTR_WINDOWS_SSH_PORT="$ssh_port" \
    "$repo_root/tools/windows/ssh-qemu.sh" 'echo SPECTR_WINDOWS_READY' >/dev/null 2>&1; do
  if (( SECONDS >= deadline )); then
    echo 'Windows SSH did not become ready; check the VM in UTM and try again.' >&2
    exit 1
  fi
  sleep 2
done

SPECTR_WINDOWS_SSH_PORT="$ssh_port" \
  SPECTR_WINDOWS_RDP_PORT="$rdp_port" \
  "$repo_root/tools/windows/ensure-rdp-over-ssh.sh" >/dev/null

# Keep the interactive guest deterministic across clones. The Windows image
# does not need a checkout: upload the small host-preparation helper over the
# already-authenticated SSH channel before opening RDP.
ssh_key="${SPECTR_WINDOWS_SSH_KEY:-$HOME/.ssh/id_ed25519_macstudio}"
[[ -f "$ssh_key" ]] || ssh_key="$HOME/.ssh/id_ed25519"
if [[ "${SPECTR_WINDOWS_PREPARE_REAPER:-0}" == 1 &&
      -f "$repo_root/tools/windows/prepare-reaper-demo.ps1" && -f "$ssh_key" ]]; then
  scp -q -P "$ssh_port" -o BatchMode=yes -o IdentitiesOnly=yes -o IdentityAgent=none \
    -o StrictHostKeyChecking=no -i "$ssh_key" \
    "$repo_root/tools/windows/prepare-reaper-demo.ps1" \
    "admin@127.0.0.1:/Users/admin/prepare-reaper-demo.ps1"
  SPECTR_WINDOWS_SSH_PORT="$ssh_port" \
    SPECTR_WINDOWS_SSH_KEY="$ssh_key" \
    "$repo_root/tools/windows/ssh-qemu.sh" \
    'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\Users\\admin\\prepare-reaper-demo.ps1 -Architecture arm64-win' >/dev/null
fi

echo "Windows RDP: 127.0.0.1:$rdp_port"
echo "Windows user: $username"
# Keep the password out of process arguments and the environment. FreeRDP
# consumes the option stream from a private inherited file descriptor.
sdl-freerdp /args-from:fd:3 3< <(
  printf '%s\n' \
    "/v:127.0.0.1:$rdp_port" "/u:$username" "/d:$rdp_domain" "/p:$password" \
    /cert:ignore /admin "/size:${SPECTR_WINDOWS_RDP_SIZE:-1920x1080}" \
    +auto-reconnect /auto-reconnect-max-retries:10 +dynamic-resolution
) &
rdp_pid=$!
# UTM may retain focus after starting the guest. Bring the visible RDP window
# forward when macOS accessibility automation is available; failure is
# harmless because Command-Tab still reaches the window.
sleep 1
osascript -e 'tell application "System Events" to set frontmost of process "sdl-freerdp" to true' >/dev/null 2>&1 || true
wait "$rdp_pid"
