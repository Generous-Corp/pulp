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
[[ -d "$backup_dir" ]] || { echo "refusing backup outside an existing disposable run: $backup_dir" >&2; exit 1; }
backup_dir_abs=$(realpath "$backup_dir")
case "$backup_dir_abs" in
  "$disposable_root_abs"|"$disposable_root_abs"/*) ;;
  *) echo "refusing backup outside disposable root: $backup_dir_abs" >&2; exit 1 ;;
esac
backup_abs="$backup_dir_abs/$(basename "$backup")"
if [[ -L "$backup_abs" ]]; then
  echo "refusing dangling backup symlink: $backup_abs" >&2
  exit 1
elif [[ -e "$backup_abs" ]]; then
  echo "refusing existing backup (must be a new file): $backup_abs" >&2
  exit 1
fi

# A running QEMU may hold the overlay open even when the guest looks idle.
# Match both the spelling in its command line and every path QEMU has open. The
# latter matters when QEMU was started through a symlink, an opaque `-blockdev`
# argument, or a relative `file=` argument. The helper is deliberately
# fail-closed when process inspection is unavailable: resizing a live disk is
# never an acceptable fallback.
python3 - "$overlay_abs" "$overlay_input_abs" <<'PY'
import pathlib, shlex, subprocess, sys

overlay = pathlib.Path(sys.argv[1]).resolve()
input_path = pathlib.Path(sys.argv[2])

def process_open_paths(pid):
    try:
        raw = subprocess.check_output(
            ["lsof", "-a", "-p", pid, "-Fn"],
            text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"refusing to inspect open QEMU paths for pid {pid}: {exc}", file=sys.stderr)
        raise SystemExit(1)
    paths = []
    cwd = None
    descriptor = None
    for line in raw.splitlines():
        if line.startswith("f"):
            descriptor = line[1:]
            continue
        if line.startswith("n"):
            name = line[1:].removesuffix(" (deleted)")
            if name:
                path = pathlib.Path(name)
                paths.append(path)
                if descriptor == "cwd":
                    cwd = path
    if not paths:
        print(f"refusing to inspect open QEMU paths for pid {pid}: lsof returned no paths", file=sys.stderr)
        raise SystemExit(1)
    return paths, cwd

try:
    listing = subprocess.check_output(
        ["ps", "-axo", "pid=,command="], text=True,
        stderr=subprocess.STDOUT)
except (OSError, subprocess.CalledProcessError) as exc:
    print(f"refusing to inspect live QEMU processes: {exc}", file=sys.stderr)
    raise SystemExit(1)

for line in listing.splitlines():
    parts = line.strip().split(None, 1)
    if len(parts) != 2 or not ("qemu-system" in parts[1] or "qemu-kvm" in parts[1]):
        continue
    pid, command = parts
    if str(overlay) in command or str(input_path) in command:
        print(f"refusing live overlay resize; QEMU pid {pid} references {overlay}", file=sys.stderr)
        raise SystemExit(1)
    try:
        tokens = shlex.split(command)
    except ValueError:
        tokens = command.split()
    open_paths, cwd = process_open_paths(pid)
    for open_path in open_paths:
        try:
            if open_path.resolve() == overlay:
                print(f"refusing live overlay resize; QEMU pid {pid} has open {open_path} resolving to {overlay}", file=sys.stderr)
                raise SystemExit(1)
        except OSError as exc:
            print(f"refusing to resolve open QEMU path for pid {pid}: {open_path}: {exc}", file=sys.stderr)
            raise SystemExit(1)
    for token in tokens:
        candidates = []
        for field in ("file=", "filename="):
            if field in token:
                candidates.append(token.split(field, 1)[1].split(",", 1)[0])
        for candidate in candidates:
            if not candidate.lower().endswith((".qcow2", ".qcow", ".img")):
                continue
            path = pathlib.Path(candidate)
            if not path.is_absolute():
                if cwd is None:
                    print(f"refusing unresolved relative QEMU disk path for pid {pid}: {candidate}", file=sys.stderr)
                    raise SystemExit(1)
                path = cwd / path
            try:
                if path.resolve() == overlay:
                    print(f"refusing live overlay resize; QEMU pid {pid} resolves {path} to {overlay}", file=sys.stderr)
                    raise SystemExit(1)
            except OSError as exc:
                print(f"refusing to resolve QEMU disk path for pid {pid}: {path}: {exc}", file=sys.stderr)
                raise SystemExit(1)
PY

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
