#!/usr/bin/env bash
set -euo pipefail

# Expand one disposable qcow2 overlay while it is offline. The Windows C:
# partition still needs to be extended with the checked-in PowerShell helper
# inside the guest after the next boot.

usage() {
  cat >&2 <<'EOF'
usage: expand-qemu-overlay.sh --overlay PATH --backup PATH --disposable-root PATH [--extra-gib N]

Creates a verified qcow2 backup, expands the named disposable overlay, and
prints the guest-side PowerShell follow-up. The guest must be shut down first.
EOF
  exit 2
}

overlay=''
backup=''
disposable_root=''
extra_gib=30
while (($#)); do
  case "$1" in
    --overlay) [[ $# -ge 2 ]] || usage; overlay=$2; shift 2 ;;
    --backup) [[ $# -ge 2 ]] || usage; backup=$2; shift 2 ;;
    --disposable-root) [[ $# -ge 2 ]] || usage; disposable_root=$2; shift 2 ;;
    --extra-gib) [[ $# -ge 2 ]] || usage; extra_gib=$2; shift 2 ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
done
[[ -n "$overlay" && -n "$backup" && -n "$disposable_root" ]] || usage
[[ "$extra_gib" =~ ^[1-9][0-9]*$ ]] || { echo 'extra GiB must be a positive integer' >&2; exit 2; }
command -v qemu-img >/dev/null || { echo 'qemu-img is required' >&2; exit 1; }
[[ -f "$overlay" ]] || { echo "overlay does not exist: $overlay" >&2; exit 1; }
[[ "$overlay" != "$backup" ]] || { echo 'backup must differ from overlay' >&2; exit 2; }

overlay_input_abs=$(cd "$(dirname "$overlay")" && printf '%s/%s' "$PWD" "$(basename "$overlay")")
overlay_abs=$(realpath "$overlay")
disposable_root_abs=$(realpath "$disposable_root")
case "$overlay_abs" in
  "$disposable_root_abs"/*) ;;
  *) echo "refusing overlay outside disposable root: $overlay_abs" >&2; exit 1 ;;
esac
backup_dir=$(dirname "$backup")
mkdir -p "$backup_dir"
backup_abs=$(cd "$backup_dir" && printf '%s/%s' "$PWD" "$(basename "$backup")")

# A running QEMU may hold the overlay open even when the guest looks idle.
# Match the complete command line and fail closed; never resize a live disk.
if ps axww -o command= | grep -E 'qemu-system|qemu-kvm' | grep -F -e "$overlay_abs" -e "$overlay_input_abs" >/dev/null; then
  echo "refusing live overlay resize; QEMU references: $overlay_abs" >&2
  exit 1
fi

# Require a real differencing image and refuse an image used by another disk.
python3 - "$overlay_abs" "$disposable_root_abs" <<'PY'
import json, pathlib, subprocess, sys
overlay = pathlib.Path(sys.argv[1]).resolve()
root = pathlib.Path(sys.argv[2]).resolve()
def image_info(path):
    try:
        raw = subprocess.check_output(
            ["qemu-img", "info", "--force-share", "--output=json", str(path)],
            text=True, stderr=subprocess.STDOUT)
        return json.loads(raw)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"refusing unreadable qcow2 metadata: {path}: {exc}", file=sys.stderr)
        raise SystemExit(1)
if not image_info(overlay).get("backing-filename"):
    print("refusing non-overlay image: qemu-img reports no backing file", file=sys.stderr)
    raise SystemExit(1)
for candidate in root.rglob("*"):
    if not candidate.is_file() or candidate.resolve() == overlay:
        continue
    if candidate.suffix.lower() not in {".qcow2", ".qcow", ".img"}:
        continue
    backing = image_info(candidate).get("backing-filename")
    if not backing:
        continue
    backing_path = pathlib.Path(backing)
    if not backing_path.is_absolute():
        backing_path = candidate.parent / backing_path
    if backing_path.resolve() == overlay:
        print(f"refusing overlay used as backing image by: {candidate}", file=sys.stderr)
        raise SystemExit(1)
PY

qemu-img check --force-share "$overlay"
qemu-img convert -p -O qcow2 "$overlay" "$backup_abs"
qemu-img check --force-share "$backup_abs"
qemu-img resize "$overlay" "+${extra_gib}G"
qemu-img info --force-share "$overlay"
qemu-img info --force-share "$backup_abs" >/dev/null

cat <<EOF
offline_overlay_resize=complete
overlay=$overlay_abs
backup=$backup_abs
extra_gib=$extra_gib
guest_followup:
  powershell -NoProfile -ExecutionPolicy Bypass -File C:\\tmp\\extend-c-volume.ps1
EOF
