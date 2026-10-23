#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
open_helper="$repo_root/tools/windows/open-utm-rdp.sh"
ssh_helper="$repo_root/tools/windows/ssh-qemu.sh"

for helper in "$open_helper" "$ssh_helper" "$repo_root/tools/windows/ensure-rdp-over-ssh.sh"; do
  bash -n "$helper"
done
bash -n "$repo_root/tools/windows/expand-qemu-overlay.sh"

# The resize helper must reject both a persistent/non-overlay image and an
# overlay outside the explicitly declared disposable root.  These are safety
# controls, not product fixtures: no resize is allowed in either case.
resize_tmp="$(mktemp -d "${TMPDIR:-/tmp}/pulp-windows-resize.XXXXXX")"
trap 'rm -f "$resize_tmp"/*.qcow2 "$resize_tmp"/*.img; rmdir "$resize_tmp" 2>/dev/null || true' EXIT
qemu-img create -q -f qcow2 "$resize_tmp/base.qcow2" 16M
non_overlay_output="$({ "$repo_root/tools/windows/expand-qemu-overlay.sh" \
  --overlay "$resize_tmp/base.qcow2" \
  --backup "$resize_tmp/base-backup.qcow2" \
  --disposable-root "$resize_tmp"; } 2>&1 || true)"
[[ "$non_overlay_output" == *"non-overlay image"* ]] || {
  echo "expand-qemu-overlay.sh accepted a non-overlay image" >&2
  exit 1
}
outside="$resize_tmp-outside.qcow2"
qemu-img create -q -f qcow2 "$outside" 16M
outside_output="$({ "$repo_root/tools/windows/expand-qemu-overlay.sh" \
  --overlay "$outside" \
  --backup "$resize_tmp/outside-backup.qcow2" \
  --disposable-root "$resize_tmp"; } 2>&1 || true)"
[[ "$outside_output" == *"outside disposable root"* ]] || {
  echo "expand-qemu-overlay.sh accepted an out-of-root image" >&2
  exit 1
}
outside_backup="$resize_tmp-outside-backup.qcow2"
outside_backup_output="$({ "$repo_root/tools/windows/expand-qemu-overlay.sh" \
  --overlay "$resize_tmp/base.qcow2" \
  --backup "$outside_backup" \
  --disposable-root "$resize_tmp"; } 2>&1 || true)"
[[ "$outside_backup_output" == *"backup outside disposable root"* ]] || {
  echo "expand-qemu-overlay.sh accepted an out-of-root backup" >&2
  exit 1
}
ln -s "$resize_tmp-outside-dangling-target.qcow2" "$resize_tmp/dangling-backup.qcow2"
dangling_output="$({ "$repo_root/tools/windows/expand-qemu-overlay.sh" \
  --overlay "$resize_tmp/base.qcow2" \
  --backup "$resize_tmp/dangling-backup.qcow2" \
  --disposable-root "$resize_tmp"; } 2>&1 || true)"
[[ "$dangling_output" == *"dangling backup symlink"* ]] || {
  echo "expand-qemu-overlay.sh accepted a dangling backup symlink" >&2
  exit 1
}
printf 'existing\n' > "$resize_tmp/existing-backup.qcow2"
existing_output="$({ "$repo_root/tools/windows/expand-qemu-overlay.sh" \
  --overlay "$resize_tmp/base.qcow2" \
  --backup "$resize_tmp/existing-backup.qcow2" \
  --disposable-root "$resize_tmp"; } 2>&1 || true)"
[[ "$existing_output" == *"existing backup"* ]] || {
  echo "expand-qemu-overlay.sh overwrote an existing backup" >&2
  exit 1
}
rm -f "$outside"
qemu-img create -q -f raw "$resize_tmp/backing.img" 16M
qemu-img create -q -f qcow2 -F raw -b "$resize_tmp/backing.img" "$resize_tmp/overlay.qcow2"
qemu-img create -q -f qcow2 -F qcow2 -b "$resize_tmp/overlay.qcow2" "$resize_tmp/child.qcow2"
dependent_output="$({ "$repo_root/tools/windows/expand-qemu-overlay.sh" \
  --overlay "$resize_tmp/overlay.qcow2" \
  --backup "$resize_tmp/overlay-backup.qcow2" \
  --disposable-root "$resize_tmp"; } 2>&1 || true)"
[[ "$dependent_output" == *"used as backing image"* ]] || {
  echo "expand-qemu-overlay.sh accepted an overlay used by another image" >&2
  exit 1
}

# The live-process guard must resolve a symlinked QEMU `file=` argument, not
# only compare the spelling printed by ps.  Inject a tiny ps/lsof fixture so no
# real VM is touched while this negative control exercises the realpath branch.
live_link="$resize_tmp/live-link.qcow2"
ln -s "$resize_tmp/overlay.qcow2" "$live_link"
fakebin="$resize_tmp/fakebin"
mkdir -p "$fakebin"
cat >"$fakebin/ps" <<EOF
#!/bin/sh
echo "4242 qemu-system-x86_64 -drive file=$live_link,if=none"
EOF
cat >"$fakebin/lsof" <<'EOF'
#!/bin/sh
echo 'ncwd'
EOF
chmod +x "$fakebin/ps" "$fakebin/lsof"
live_output="$({ PATH="$fakebin:$PATH" "$repo_root/tools/windows/expand-qemu-overlay.sh" \
  --overlay "$resize_tmp/overlay.qcow2" \
  --backup "$resize_tmp/live-backup.qcow2" \
  --disposable-root "$resize_tmp"; } 2>&1 || true)"
[[ "$live_output" == *"live overlay resize"* ]] || {
  echo "expand-qemu-overlay.sh accepted a symlinked live QEMU image" >&2
  exit 1
}
cat >"$fakebin/ps" <<'EOF'
#!/bin/sh
echo '4243 qemu-system-x86_64 -drive file=overlay.qcow2,if=none'
EOF
cat >"$fakebin/lsof" <<EOF
#!/bin/sh
echo 'n$resize_tmp'
EOF
relative_output="$({ PATH="$fakebin:$PATH" "$repo_root/tools/windows/expand-qemu-overlay.sh" \
  --overlay "$resize_tmp/overlay.qcow2" \
  --backup "$resize_tmp/relative-backup.qcow2" \
  --disposable-root "$resize_tmp"; } 2>&1 || true)"
[[ "$relative_output" == *"live overlay resize"* ]] || {
  echo "expand-qemu-overlay.sh accepted a relative live QEMU image" >&2
  exit 1
}
cat >"$fakebin/ps" <<EOF
#!/bin/sh
echo "4244 qemu-system-aarch64 -blockdev driver=file,filename=$live_link,locking=off"
EOF
blockdev_output="$({ PATH="$fakebin:$PATH" "$repo_root/tools/windows/expand-qemu-overlay.sh" \
  --overlay "$resize_tmp/overlay.qcow2" \
  --backup "$resize_tmp/blockdev-backup.qcow2" \
  --disposable-root "$resize_tmp"; } 2>&1 || true)"
[[ "$blockdev_output" == *"live overlay resize"* ]] || {
  echo "expand-qemu-overlay.sh accepted a -blockdev filename live image" >&2
  exit 1
}

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
