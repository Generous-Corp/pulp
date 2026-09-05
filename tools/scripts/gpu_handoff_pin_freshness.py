#!/usr/bin/env python3
"""Editing a pinned path is a two-commit operation; catch the missing second one.

``docs/status/gpu-vellum-handoff.yaml`` pins every referenced Pulp path to a
revision, object id and object type. Change one of those files and the pinned
row goes stale, so ``gpu-recipe-catalog-selftest`` and
``gpu-handoff-provenance-selftest`` fail — but only in CI, roughly twenty
minutes later.

The authoritative checker, ``gpu_handoff_provenance.py check``, verifies all
three identity fields for every pinned row. That costs ~25s because it runs a
``git log`` per path, which is too slow for a gate that runs on every push.

So this guard asks the cheap question instead: *did this range touch a pinned
path without touching the ledger?* That is the shape all three PRs hit on
2026-09-05 — the ledger refresh was forgotten entirely, not merely stale. It is
a subset of what the full checker proves, and it says so rather than implying
the pins are verified.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

HANDOFF = Path("docs/status/gpu-vellum-handoff.yaml")
REPAIR = "python3 tools/scripts/gpu_handoff_provenance.py write"
VERIFY = "python3 tools/scripts/gpu_handoff_provenance.py check"


def pinned_paths(root: Path) -> set[str]:
    """Every ``entries[*].pulp_paths[*].path``.

    The file carries a ``.yaml`` extension but its contents are JSON, so it
    parses with the stdlib and needs no PyYAML — which matters because the repo
    already treats a PyYAML dependency as real friction on PEP-668 Python.
    """
    doc = root / HANDOFF
    if not doc.is_file():
        return set()
    try:
        data = json.loads(doc.read_text())
    except (OSError, json.JSONDecodeError):
        return set()
    found: set[str] = set()
    for entry in data.get("entries") or []:
        for row in entry.get("pulp_paths") or []:
            path = row.get("path") if isinstance(row, dict) else None
            if isinstance(path, str) and path:
                found.add(path)
    return found


def changed(base: str, root: Path) -> set[str]:
    proc = subprocess.run(
        ["git", "diff", "--name-only", f"{base}...HEAD"],
        cwd=root, capture_output=True, text=True, check=False,
    )
    if proc.returncode != 0:
        return set()
    return {line.strip() for line in proc.stdout.splitlines() if line.strip()}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="origin/main")
    ap.add_argument("--mode", choices=("report", "hint"), default="report")
    ap.add_argument("--root", default=".")
    args = ap.parse_args(argv)
    root = Path(args.root).resolve()

    pins = pinned_paths(root)
    if not pins:
        # No ledger, or a shape this guard cannot read. Say so rather than
        # passing silently, because a guard that cannot see its subject must
        # never look like one that checked it.
        print("gpu-handoff-pin: no pinned rows found; nothing checked", file=sys.stderr)
        return 0

    touched = changed(args.base, root)
    hits = sorted(pins & touched)
    if not hits:
        return 0
    if str(HANDOFF) in touched:
        return 0

    print("", file=sys.stderr)
    print("gpu-handoff-pin: pinned path(s) changed without refreshing the ledger:",
          file=sys.stderr)
    for h in hits:
        print(f"    {h}", file=sys.stderr)
    print(f"  {HANDOFF} pins these to an exact revision, so the rows are now stale", file=sys.stderr)
    print(f"  and gpu-recipe-catalog-selftest / gpu-handoff-provenance-selftest will fail in CI.", file=sys.stderr)
    print(f"  Repair:  {REPAIR}", file=sys.stderr)
    print(f"  Verify:  {VERIFY}", file=sys.stderr)
    print("  (this guard only checks that the ledger was touched; the verify", file=sys.stderr)
    print("   command above is what proves every identity field is correct)", file=sys.stderr)
    return 0 if args.mode == "hint" else 1


if __name__ == "__main__":
    raise SystemExit(main())
