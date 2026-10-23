#!/usr/bin/env bash
set -euo pipefail

# Expand one disposable qcow2 overlay while it is offline. The Windows C:
# partition still needs to be extended inside the guest after the next boot.

usage() {
  cat >&2 <<'EOF'
usage: expand-qemu-overlay.sh --overlay PATH --backup PATH [--extra-gib N]

Creates a verified qcow2 backup, expands the named disposable overlay, and
prints the guest-side DiskPart command. The guest must be shut down first.
EOF
  exit 2
}

overlay=''
backup=''
extra_gib=30
while (($#)); do
  case "$1" in
    --overlay) [[ $# -ge 2 ]] || usage; overlay=$2; shift 2 ;;
    --backup) [[ $# -ge 2 ]] || usage; backup=$2; shift 2 ;;
    --extra-gib) [[ $# -ge 2 ]] || usage; extra_gib=$2; shift 2 ;;
    -h|--help) usage ;;
    *) usage ;;
  esac
done
[[ -n "$overlay" && -n "$backup" ]] || usage
[[ "$extra_gib" =~ ^[1-9][0-9]*$ ]] || { echo 'extra GiB must be a positive integer' >&2; exit 2; }
command -v qemu-img >/dev/null || { echo 'qemu-img is required' >&2; exit 1; }
[[ -f "$overlay" ]] || { echo "overlay does not exist: $overlay" >&2; exit 1; }
[[ "$overlay" != "$backup" ]] || { echo 'backup must differ from overlay' >&2; exit 2; }

overlay_abs=$(cd "$(dirname "$overlay")" && printf '%s/%s' "$PWD" "$(basename "$overlay")")
backup_dir=$(dirname "$backup")
mkdir -p "$backup_dir"
backup_abs=$(cd "$backup_dir" && printf '%s/%s' "$PWD" "$(basename "$backup")")

# A running QEMU may hold the overlay open even when the guest looks idle.
# Match the complete command line and fail closed; never resize a live disk.
if ps axww -o command= | grep -F -- "$overlay_abs" | grep -E 'qemu-system|qemu-kvm' | grep -v 'grep -F' >/dev/null; then
  echo "refusing live overlay resize; QEMU references: $overlay_abs" >&2
  exit 1
fi

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
  diskpart /s C:\\tmp\\extend-c.txt
  (echo select volume C & echo extend) > C:\\tmp\\extend-c.txt
EOF
