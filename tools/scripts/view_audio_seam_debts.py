#!/usr/bin/env python3
"""Hold docs/status/view-audio-seam-debts.json to the tree it describes.

The view-audio seam leaves two named debts in headers the Vellum package ships:
the guarded audio-widget include at the end of widgets.hpp, and the Meter /
WaveformView forward declarations with their deprecated bind virtuals in
design_import.hpp. Each debt row names the exact lines it covers. A row whose
lines are gone is stale and fails, so the record and the headers are retired
together; a covered line that changes shape fails the same way.

The rows are deferred on the condition that a Vellum-package sterile consumer
proves them inert. That proof cannot run until the package exists, so it is
recorded here as pending with its four checks, and this gate requires the
record to cover exactly the recorded debts.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

LEDGER = "docs/status/view-audio-seam-debts.json"
EXPECTED_CHECKS = {
    "no-widgets-gate-definition",
    "no-audio-headers",
    "no-audio-symbols",
    "name-only-forward-declarations",
}


def problems(root: pathlib.Path) -> list[str]:
    found: list[str] = []
    ledger = json.loads((root / LEDGER).read_text(encoding="utf-8"))
    debt_ids: set[str] = set()
    for entry in ledger.get("entries", []):
        debt_id = entry.get("id", "")
        debt_ids.add(debt_id)
        path = root / entry.get("file", "")
        if not path.is_file():
            found.append(f"{debt_id}: {entry.get('file')} does not exist")
            continue
        text = path.read_text(encoding="utf-8")
        patterns = entry.get("patterns", [])
        if not patterns:
            found.append(f"{debt_id}: no patterns")
        for pattern in patterns:
            if not re.search(pattern, text, re.MULTILINE):
                found.append(f"{debt_id}: stale, {entry['file']} has no line matching {pattern!r}")
    if not debt_ids:
        found.append("no debt entries")

    pending = {p.get("id"): p for p in ledger.get("pending_proofs", [])}
    proof = pending.get("vellum-package-inertness")
    if proof is None:
        found.append("the Vellum-package inertness proof is not recorded")
    else:
        if proof.get("status") != "pending":
            found.append(f"inertness proof status is {proof.get('status')!r}, expected 'pending'")
        if set(proof.get("debts", [])) != debt_ids:
            found.append(
                f"inertness proof covers {sorted(proof.get('debts', []))}, debts are {sorted(debt_ids)}")
        checks = {c.get("id") for c in proof.get("checks", []) if c.get("assert")}
        if checks != EXPECTED_CHECKS:
            found.append(f"inertness proof checks are {sorted(checks)}, expected {sorted(EXPECTED_CHECKS)}")
    return found


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    args = parser.parse_args()
    found = problems(args.root)
    for problem in found:
        print(f"view-audio-seam-debts: {problem}", file=sys.stderr)
    if found:
        return 1
    print("view-audio-seam-debts: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
