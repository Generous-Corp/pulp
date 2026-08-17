#!/usr/bin/env python3
"""Aggregate receipt audit — the fleet-wide view no single host can produce.

A host's watchdog can answer "am *I* converged?". It cannot answer "is the
FLEET converged?", because it cannot see the other hosts' receipts. This script
answers the second question by comparing committed receipts against the
manifest roster and epoch.

THE RECEIPTS RULE: N hosts declared means N receipts at the current epoch. A
host with no receipt is not "probably fine" -- it is UNCONVERGED, and it counts
against the fleet exactly as loudly as a host with a failing one. That rule is
what stops a rollout from being declared done because the hosts that reported
happened to be the healthy ones.

exit 0  converged   every declared host has a receipt at the current epoch, all clean
exit 1  lagging     at least one host is missing or behind, or reported non-clean
exit 3  usage / manifest error
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fleet_lib as F  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", default=None)
    ap.add_argument("--receipts-dir", default=None)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    manifest_path = args.manifest or os.path.join(repo_root, "planning/fleet/manifest.toml")
    if not os.path.exists(manifest_path):
        print(f"receipts_audit: manifest not found: {manifest_path}", file=sys.stderr)
        print("receipts_audit: run `git submodule update --init planning`", file=sys.stderr)
        return 3
    try:
        manifest = F.load_manifest(manifest_path)
    except Exception as exc:
        print(f"receipts_audit: {exc}", file=sys.stderr)
        return 3

    epoch = manifest["epoch"]
    receipts_dir = args.receipts_dir or os.path.join(
        repo_root, manifest.get("meta", {}).get("receipts_dir", "planning/fleet/receipts"))

    rows, missing, behind, unclean = [], [], [], []
    for host_id in manifest["hosts"]:
        path = os.path.join(receipts_dir, f"{host_id}.json")
        if not os.path.exists(path):
            missing.append(host_id)
            rows.append({"host": host_id, "state": "missing", "epoch": None,
                         "verdict": None, "generated_at": None})
            continue
        try:
            r = json.load(open(path))
        except Exception as exc:
            # An unparseable receipt is NOT a pass. Treat it as missing.
            missing.append(host_id)
            rows.append({"host": host_id, "state": f"unreadable ({exc})", "epoch": None,
                         "verdict": None, "generated_at": None})
            continue
        got_epoch = r.get("epoch")
        verdict = r.get("verdict")
        state = "ok"
        if got_epoch != epoch:
            behind.append(host_id)
            state = f"behind (epoch {got_epoch} < {epoch})"
        elif verdict != "clean":
            unclean.append(host_id)
            state = f"reported {verdict}"
        rows.append({"host": host_id, "state": state, "epoch": got_epoch,
                     "verdict": verdict, "generated_at": r.get("generated_at")})

    converged = not (missing or behind or unclean)
    payload = {
        "converged": converged, "epoch": epoch, "hosts": len(manifest["hosts"]),
        "receipts_at_epoch": sum(1 for r in rows if r["epoch"] == epoch),
        "missing": missing, "behind": behind, "unclean": unclean, "rows": rows,
    }

    if args.json:
        print(json.dumps(payload, indent=2))
    else:
        print(f"fleet epoch {epoch} — {payload['receipts_at_epoch']}/{payload['hosts']} "
              f"hosts have a receipt at this epoch")
        for r in rows:
            mark = "  OK  " if r["state"] == "ok" else " LAG  "
            print(f"{mark}  {r['host']:<12} {r['state']}"
                  + (f"   ({r['generated_at']})" if r["generated_at"] else ""))
        if missing:
            print(f"\nMISSING receipts: {', '.join(missing)}")
            print("  A host with no receipt is UNCONVERGED, not assumed-healthy.")
        if behind:
            print(f"BEHIND: {', '.join(behind)}")
        if unclean:
            print(f"NON-CLEAN: {', '.join(unclean)}")
        print(f"\nVERDICT={'converged' if converged else 'lagging'}")

    return 0 if converged else 1


if __name__ == "__main__":
    sys.exit(main())
