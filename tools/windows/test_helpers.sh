#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
open_helper="$repo_root/tools/windows/open-utm-rdp.sh"
ssh_helper="$repo_root/tools/windows/ssh-qemu.sh"

for helper in "$open_helper" "$ssh_helper" "$repo_root/tools/windows/ensure-rdp-over-ssh.sh"; do
  bash -n "$helper"
done

missing_config_output="$({ env -u SPECTR_WINDOWS_UTM_UUID -u SPECTR_WINDOWS_SSH_PORT \
  -u SPECTR_WINDOWS_RDP_PORT \
  SPECTR_WINDOWS_UTM_CONFIG="${TMPDIR:-/tmp}/pulp-windows-helper-config-does-not-exist" \
  "$open_helper"; } 2>&1 || true)"
[[ "$missing_config_output" == *"set guest values"* ]] || {
  echo "open-utm-rdp.sh did not reject missing guest identity" >&2
  exit 1
}

missing_secret_dir="$(mktemp -d "${TMPDIR:-/tmp}/pulp-windows-helper.XXXXXX")"
missing_secret="$missing_secret_dir/secret"
config_file="$missing_secret_dir/config"
printf '%s\n' \
  'SPECTR_WINDOWS_UTM_UUID=fixture' \
  'SPECTR_WINDOWS_SSH_PORT=1' \
  'SPECTR_WINDOWS_RDP_PORT=2' >"$config_file"
trap 'rm -f "$missing_secret" "$config_file"; rmdir "$missing_secret_dir" 2>/dev/null || true' EXIT
missing_secret_output="$({ \
  SPECTR_WINDOWS_UTM_CONFIG="$config_file" \
  SPECTR_WINDOWS_UTM_SECRET="$missing_secret" \
  "$open_helper"; } 2>&1 || true)"
[[ "$missing_secret_output" == *"missing credential file"* ]] || {
  echo "open-utm-rdp.sh did not reject a missing credential file" >&2
  exit 1
}

missing_key_output="$({ \
  SPECTR_WINDOWS_SSH_KEY="$missing_secret" \
  "$ssh_helper" true; } 2>&1 || true)"
[[ "$missing_key_output" == *"missing SSH key"* ]] || {
  echo "ssh-qemu.sh did not reject a missing key" >&2
  exit 1
}

echo "windows helper safety checks passed"
