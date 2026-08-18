#!/usr/bin/env bash
# Three-valued verification of this host against the fleet convergence manifest.
#
#   exit 0  VERDICT=compliant     every declared key was observed and matches
#   exit 1  VERDICT=drifted       every key was observed; some do not match
#   exit 2  VERDICT=unobservable  at least one key could NOT be observed
#   exit 3  usage / manifest error
#
# PRECEDENCE: unobservable beats drifted. If a run finds both, it exits 2, not
# 1. Exit 1 is a claim that the host was fully inspected and here are the exact
# deltas. When any probe failed, that claim is false, and reporting 1 would let
# a caller believe a bounded fix-list exists when part of the host is unknown.
#
# The one thing this script must never do is report compliant for a key it could
# not read. See tools/fleet/fleet_lib.py for the probe-level contract.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
MANIFEST="${FLEET_MANIFEST:-$REPO_ROOT/planning/fleet/manifest.toml}"
HOST_OVERRIDE=""
JSON=0

usage() {
  cat <<'EOF'
usage: verify.sh [--manifest PATH] [--host ID] [--json]

  --manifest PATH  manifest to verify against (default: <repo>/planning/fleet/manifest.toml,
                   override with $FLEET_MANIFEST)
  --host ID        verify as this manifest host instead of auto-detecting by hostname
  --json           emit a JSON object instead of the human/machine line format

exit: 0 compliant | 1 drifted | 2 unobservable | 3 usage or manifest error
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --manifest) MANIFEST="${2:-}"; shift 2 || { usage >&2; exit 3; } ;;
    --host)     HOST_OVERRIDE="${2:-}"; shift 2 || { usage >&2; exit 3; } ;;
    --json)     JSON=1; shift ;;
    -h|--help)  usage; exit 0 ;;
    *) echo "verify.sh: unknown argument: $1" >&2; usage >&2; exit 3 ;;
  esac
done

if [ ! -f "$MANIFEST" ]; then
  # A missing manifest is exit 3, deliberately NOT exit 0. The planning
  # submodule is optional for external cloners, so "no manifest" is a normal
  # state for them -- but it must never read as "this host is compliant".
  echo "verify.sh: manifest not found: $MANIFEST" >&2
  echo "verify.sh: if this is a fresh worktree, run: git submodule update --init planning" >&2
  exit 3
fi

command -v python3 >/dev/null 2>&1 || { echo "verify.sh: python3 is required" >&2; exit 3; }
if ! python3 -c 'import tomllib' 2>/dev/null && ! python3 -c 'import tomli' 2>/dev/null; then
  echo "verify.sh: this python3 ($(command -v python3)) has neither tomllib (>= 3.11) nor tomli" >&2
  echo "verify.sh: fix with either: brew install python@3.11   OR   python3 -m pip install --user tomli" >&2
  echo "verify.sh: refusing to run -- a parser failure must not be reported as a verdict" >&2
  exit 3
fi

# Run the probe engine into a buffer rather than straight to stdout.
#
# The exit code alone is NOT trustworthy here: an uncaught exception in the
# python block also exits 1, which is the code for `drifted`. A crash that
# reads as a verdict is precisely the failure this tool exists to prevent, so
# a run only counts as a verdict when it actually PRINTED one. Anything else
# is a harness error (exit 3), never a statement about the host.
OUT="$(mktemp "${TMPDIR:-/tmp}/fleet-verify.XXXXXX")" || { echo "verify.sh: cannot create temp file" >&2; exit 3; }
trap 'rm -f "$OUT"' EXIT

set +e
FLEET_MANIFEST="$MANIFEST" \
FLEET_HOST_OVERRIDE="$HOST_OVERRIDE" \
FLEET_JSON="$JSON" \
python3 - "$SCRIPT_DIR" >"$OUT" 2>&1 <<'PY'
import json, os, sys
sys.path.insert(0, sys.argv[1])
import fleet_lib as F

manifest_path = os.environ["FLEET_MANIFEST"]
override = os.environ.get("FLEET_HOST_OVERRIDE") or None
as_json = os.environ.get("FLEET_JSON") == "1"

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

rows, drifted, unobs = [], [], []
for key in F.keys_for(manifest, host_id):
    pr = F.probe(key)
    rows.append({
        "key": key["name"], "kind": key.get("kind"), "state": pr.state,
        "declared": str(key.get("value", "")), "observed": pr.observed,
        "detail": pr.detail, "declared_verified": bool(key.get("verified", False)),
    })
    if pr.state == F.DRIFT:
        drifted.append(key["name"])
    elif pr.state == F.UNOBS:
        unobs.append(key["name"])

# Precedence: unobservable dominates drifted dominates compliant.
if unobs:
    verdict, code = "unobservable", 2
elif drifted:
    verdict, code = "drifted", 1
else:
    verdict, code = "compliant", 0

payload = {
    "verdict": verdict, "host": host_id, "epoch": manifest["epoch"],
    "manifest": manifest_path, "drifted": drifted, "unobservable": unobs,
    "keys": rows,
}

if as_json:
    print(json.dumps(payload, indent=2))
else:
    print(f"host={host_id} epoch={manifest['epoch']} manifest={manifest_path}")
    for r in rows:
        mark = {"ok": "  OK  ", "drift": " DRIFT", "unobservable": " UNOBS"}[r["state"]]
        line = f"{mark}  {r['key']} ({r['kind']})"
        if r["state"] != "ok":
            line += f"\n          declared: {r['declared']}"
            line += f"\n          observed: {r['observed']}"
            if r["detail"]:
                line += f"\n          detail:   {r['detail']}"
            if not r["declared_verified"]:
                line += "\n          note:     declared value is UNVERIFIED -- the manifest may be wrong, not the host"
        print(line)
    print(f"VERDICT={verdict}")
    print(f"DRIFTED_KEYS={','.join(drifted)}")
    print(f"UNOBSERVABLE_KEYS={','.join(unobs)}")

sys.exit(code)
PY
RC=$?
set -e

cat "$OUT"

# A real verdict always prints its marker. JSON mode prints a "verdict" field.
if [ "$JSON" -eq 1 ]; then
  MARKER='"verdict"'
else
  MARKER='VERDICT='
fi

if ! grep -q -- "$MARKER" "$OUT"; then
  echo "verify.sh: probe engine exited $RC without producing a verdict -- treating as a harness error, NOT as a result about this host" >&2
  exit 3
fi

case "$RC" in
  0|1|2) exit "$RC" ;;
  *) echo "verify.sh: probe engine exited $RC (unexpected)" >&2; exit 3 ;;
esac
