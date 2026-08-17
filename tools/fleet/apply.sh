#!/usr/bin/env bash
# Converge this host toward the fleet convergence manifest, then write a receipt.
#
# IDEMPOTENT BY CONSTRUCTION: every action is a converge-to-declared-state
# operation (mkdir -p, write-exact-content), never an append or a toggle. The
# second run of apply.sh finds every key already OK and reports `ok` where the
# first reported `fixed`. Nothing is re-done.
#
# Keys with apply="none" are NEVER mutated. Rewriting a live runner's launchd
# plist or a GitHub routing variable unattended can re-point a running required
# gate; those are reported `manual` and left for the watchdog to escalate.
#
#   exit 0  every key ended ok/fixed
#   exit 1  at least one key ended manual (drifted, not auto-fixable)
#   exit 2  at least one key was unobservable
#   exit 4  at least one key FAILED to apply
#   exit 3  usage / manifest error
#
# Precedence: failed > unobservable > manual > clean.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
MANIFEST="${FLEET_MANIFEST:-$REPO_ROOT/planning/fleet/manifest.toml}"
HOST_OVERRIDE=""
RECEIPTS_DIR="${FLEET_RECEIPTS_DIR:-}"
DRY_RUN=0

usage() {
  cat <<'EOF'
usage: apply.sh [--manifest PATH] [--host ID] [--receipts-dir PATH] [--dry-run]

  --manifest PATH     manifest to apply (default: <repo>/planning/fleet/manifest.toml)
  --host ID           apply as this manifest host instead of auto-detecting
  --receipts-dir PATH where to write the receipt (default: manifest [meta].receipts_dir)
  --dry-run           probe and report, mutate nothing, write no receipt

exit: 0 clean | 1 manual keys | 2 unobservable | 4 apply failed | 3 usage/manifest error
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --manifest)      MANIFEST="${2:-}"; shift 2 || { usage >&2; exit 3; } ;;
    --host)          HOST_OVERRIDE="${2:-}"; shift 2 || { usage >&2; exit 3; } ;;
    --receipts-dir)  RECEIPTS_DIR="${2:-}"; shift 2 || { usage >&2; exit 3; } ;;
    --dry-run)       DRY_RUN=1; shift ;;
    -h|--help)       usage; exit 0 ;;
    *) echo "apply.sh: unknown argument: $1" >&2; usage >&2; exit 3 ;;
  esac
done

if [ ! -f "$MANIFEST" ]; then
  echo "apply.sh: manifest not found: $MANIFEST" >&2
  echo "apply.sh: if this is a fresh worktree, run: git submodule update --init planning" >&2
  exit 3
fi

command -v python3 >/dev/null 2>&1 || { echo "apply.sh: python3 is required" >&2; exit 3; }
if ! python3 -c 'import tomllib' 2>/dev/null && ! python3 -c 'import tomli' 2>/dev/null; then
  echo "apply.sh: this python3 ($(command -v python3)) has neither tomllib (>= 3.11) nor tomli" >&2
  echo "apply.sh: fix with either: brew install python@3.11   OR   python3 -m pip install --user tomli" >&2
  exit 3
fi

# Same trap as verify.sh: an uncaught exception exits 1, which is also the code
# for "some keys need manual attention". A crash must never read as a result.
OUT="$(mktemp "${TMPDIR:-/tmp}/fleet-apply.XXXXXX")" || { echo "apply.sh: cannot create temp file" >&2; exit 3; }
trap 'rm -f "$OUT"' EXIT

set +e
FLEET_MANIFEST="$MANIFEST" \
FLEET_HOST_OVERRIDE="$HOST_OVERRIDE" \
FLEET_RECEIPTS_DIR="$RECEIPTS_DIR" \
FLEET_REPO_ROOT="$REPO_ROOT" \
FLEET_DRY_RUN="$DRY_RUN" \
python3 - "$SCRIPT_DIR" >"$OUT" 2>&1 <<'PY'
import datetime, json, os, subprocess, sys
sys.path.insert(0, sys.argv[1])
import fleet_lib as F

manifest_path = os.environ["FLEET_MANIFEST"]
override = os.environ.get("FLEET_HOST_OVERRIDE") or None
dry_run = os.environ.get("FLEET_DRY_RUN") == "1"
repo_root = os.environ["FLEET_REPO_ROOT"]

try:
    manifest = F.load_manifest(manifest_path)
except Exception as exc:
    F.die(f"cannot load manifest: {exc}", 3)

try:
    host_id = F.identify_host(manifest, override)
except ValueError as exc:
    F.die(str(exc), 3)
if host_id is None:
    F.die("this machine matches no host in the manifest (add it, or pass --host)", 3)

meta = manifest.get("meta", {})
receipts_dir = os.environ.get("FLEET_RECEIPTS_DIR") or os.path.join(
    repo_root, meta.get("receipts_dir", "planning/fleet/receipts")
)

try:
    hostname = subprocess.run(["hostname", "-s"], capture_output=True, text=True,
                              timeout=10, check=True).stdout.strip()
except Exception:
    hostname = "unknown"

results, counts = [], {"ok": 0, "fixed": 0, "manual": 0, "failed": 0, "unobservable": 0}
for key in F.keys_for(manifest, host_id):
    pr = F.probe(key)
    if dry_run:
        outcome = {"ok": "ok", "drift": "would-apply", "unobservable": "unobservable"}[pr.state]
        detail = pr.detail
        if pr.state == F.DRIFT and key.get("apply", "none") == "none":
            outcome = "manual"
    else:
        outcome, detail = F.apply_key(key, pr)
    counts[outcome] = counts.get(outcome, 0) + 1
    results.append({
        "key": key["name"], "kind": key.get("kind"), "outcome": outcome,
        "declared": str(key.get("value", "")), "observed_before": pr.observed,
        "detail": detail, "declared_verified": bool(key.get("verified", False)),
    })
    print(f"  {outcome:<13} {key['name']}" + (f"  ({detail})" if detail else ""))

# Receipt. `generated_at` sits outside `body` so a timestamp alone does not make
# two runs look different.
#
# But do NOT read that as "a converged re-run produces a byte-identical body".
# It does not, and testing for that gives a false failure: `observed_before`
# records a live measurement, and a disk-floor key reports e.g. 337.5GB then
# 337.4GB seconds later because the machine is in use. That is the receipt being
# honest, not apply.sh being non-idempotent.
#
# Idempotence here is a property of OUTCOMES, not of bytes: a second run must
# fix nothing (`counts["fixed"] == 0`) and must not change any key's `outcome`.
# Assert that. Receipt mtime is likewise not a signal -- the file is rewritten
# unconditionally every run.
body = {
    "epoch": manifest["epoch"], "host": host_id, "hostname": hostname,
    "manifest": os.path.relpath(manifest_path, repo_root),
    "keys": results, "counts": counts,
}

if counts.get("failed"):
    verdict, code = "failed", 4
elif counts.get("unobservable"):
    verdict, code = "unobservable", 2
elif counts.get("manual"):
    verdict, code = "manual", 1
else:
    verdict, code = "clean", 0
body["verdict"] = verdict

if dry_run:
    print(f"\nDRY RUN — no receipt written. verdict={verdict}")
    sys.exit(code)

os.makedirs(receipts_dir, exist_ok=True)
receipt_path = os.path.join(receipts_dir, f"{host_id}.json")
receipt = dict(body)
receipt["generated_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat(
    timespec="seconds").replace("+00:00", "Z")
with open(receipt_path, "w", encoding="utf-8") as fh:
    json.dump(receipt, fh, indent=2, sort_keys=True)
    fh.write("\n")

print(f"\nreceipt: {receipt_path}")

# Second sink: Shipyard's Layer C fail-closed gate reads a per-host TOML receipt
# from an uncommitted local path, keyed by `hostname -s`. That is a different
# consumer from the committed fleet audit above -- on-host and local, versus
# shared and aggregated -- so it needs its own file, but NOT its own source of
# truth. Both are serialized from `body` in this one block so a single run
# cannot emit two disagreeing receipts.
#
# The receipt carries BOTH keys: `host` (the manifest's stable logical id) and
# `hostname` (the raw machine name this file is named for). A machine that gets
# renamed then writes a new filename while the old one sits there looking merely
# stale; `host` is what lets a consumer notice they are the same machine.
def toml_scalar(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    # TOML basic strings accept the same escapes JSON emits (\" \\ \n \uXXXX),
    # so json.dumps is a correct encoder here and avoids hand-rolling escaping.
    return json.dumps(str(v))

shipyard_dir = os.environ.get("FLEET_SHIPYARD_RECEIPTS_DIR") or os.path.join(
    repo_root, ".shipyard.local", "fleet-receipts")
os.makedirs(shipyard_dir, exist_ok=True)
shipyard_path = os.path.join(shipyard_dir, f"{hostname}.toml")

lines = ["# Generated by tools/fleet/apply.sh -- do not hand-edit.",
         "# Regenerate with: tools/fleet/apply.sh", ""]
for field in ("epoch", "host", "hostname", "verdict", "manifest", "generated_at"):
    lines.append(f"{field} = {toml_scalar(receipt[field])}")
lines += ["", "[counts]"]
for k in sorted(counts):
    lines.append(f"{k} = {toml_scalar(counts[k])}")
for r in results:
    lines += ["", "[[keys]]",
              f"name = {toml_scalar(r['key'])}",
              f"outcome = {toml_scalar(r['outcome'])}",
              f"declared_verified = {toml_scalar(r['declared_verified'])}"]
with open(shipyard_path, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines) + "\n")

print(f"receipt: {shipyard_path}  (Shipyard Layer C)")
print(f"VERDICT={verdict} epoch={manifest['epoch']} host={host_id}")
sys.exit(code)
PY
RC=$?
set -e

cat "$OUT"

if ! grep -qE 'VERDICT=|DRY RUN' "$OUT"; then
  echo "apply.sh: engine exited $RC without producing a verdict -- treating as a harness error, NOT as a result about this host" >&2
  exit 3
fi

case "$RC" in
  0|1|2|4) exit "$RC" ;;
  *) echo "apply.sh: engine exited $RC (unexpected)" >&2; exit 3 ;;
esac
