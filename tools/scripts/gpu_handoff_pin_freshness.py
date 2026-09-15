#!/usr/bin/env python3
"""Editing a pinned path is a two-commit operation; catch the missing second one.

``docs/status/gpu-vellum-handoff.yaml`` pins every referenced Pulp path to a
revision, object id and object type. Change one of those files without
regenerating and the pinned row stops describing HEAD.

What that costs has narrowed since this guard landed, and the honest version
matters for judging whether the guard still earns its place. Staleness of that
kind now falls entirely on the *currency* tier, which is opt-in behind
``PULP_GPU_HANDOFF_REQUIRE_CURRENT=1`` — nothing in ``.github`` sets it — so it
no longer turns the required gate red twenty minutes later. The always-on
provenance tier is unaffected, because a pin that has merely fallen behind is
still ancestral and still names a real object.

So the drift this guard catches is quiet rather than loud: the ledger keeps
claiming an identity the repository has moved past, and every consumer that
asks for the stronger claim — ``gpu_handoff_provenance.py check``, or any
reader of the published receipt — gets an answer that is wrong about today.
Catching that at push is worth a sub-second check precisely because nothing
downstream is going to shout about it.

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
# --receipt is not optional in practice: the published receipt binds itself to
# the ledger's exact bytes, so regenerating one without the other leaves the
# receipt claiming a source that no longer exists.
REPAIR = "python3 tools/scripts/gpu_handoff_provenance.py write --receipt"
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
    print(f"  {HANDOFF} pins these to an exact revision, so the rows now describe", file=sys.stderr)
    print("  a revision the repository has moved past. The required gate will not say", file=sys.stderr)
    print("  so — currency is opt-in — which is why this is caught here instead.", file=sys.stderr)
    print(f"  Repair:  {REPAIR}", file=sys.stderr)
    print(f"  Verify:  {VERIFY}", file=sys.stderr)
    print("  (this guard only checks that the ledger was touched; the verify", file=sys.stderr)
    print("   command above is what proves every identity field is correct)", file=sys.stderr)
    return 0 if args.mode == "hint" else 1


if __name__ == "__main__":
    raise SystemExit(main())
