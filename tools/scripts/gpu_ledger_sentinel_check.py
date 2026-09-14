#!/usr/bin/env python3
"""Reject a generated ledger the merge driver resolved but nobody regenerated.

``docs/status/gpu-vellum-handoff.yaml`` and its receipt are regenerated from one
exact commit whenever main moves, so every branch that outlives a main move
collides on them. ``tools/scripts/gpu_ledger_merge_driver.sh`` resolves that
collision to the identity ``regenerate-me`` instead of to a merged value,
because every value that *looks* like an identity is accepted somewhere: a
stale-but-ancestral pin satisfies the always-on provenance tier, so a driver
that computed the merge -- or ``merge=ours`` -- would hand Git a wrong answer it
would commit without a word.

An invalid value only helps if something invalid-aware is looking, and the
guards that already exist do not look here:

* ``gpu_handoff_pin_freshness.py`` fires when a pinned path changes and the
  ledger does *not*. A sentinel merge changes the ledger, so it reads the
  sentinel as the refresh it was waiting for and stays quiet.
* ``conflict_marker_check.py`` looks for ``<<<<<<<``. The driver's entire
  purpose is that there are none.

Which leaves the required CI gate, twenty minutes downstream -- the roundtrip
the driver exists to remove. So this is the check that reads the merged bytes
and says no. It is a literal substring scan over two files: no imports beyond
the stdlib, no Git, no parsing, fast enough for the pre-push path.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Must equal the value tools/scripts/gpu_ledger_merge_driver.sh substitutes.
# The driver cannot import this module -- Git runs it as a bare shell command in
# the middle of a merge -- so the two literals are kept in step by a test rather
# than by construction.
SENTINEL = "regenerate-me"

LEDGERS = (
    Path("docs/status/gpu-vellum-handoff.yaml"),
    Path("docs/validation/gpu-handoff-provenance/receipt.json"),
)

# Both files are regenerated as a unit and the receipt binds itself to the
# ledger's exact bytes, so dropping --receipt repairs one and leaves the other
# describing bytes that no longer exist -- green here, red in CI.
REPAIR = "python3 tools/scripts/gpu_handoff_provenance.py write --source-commit HEAD --receipt"
VERIFY = "python3 tools/scripts/gpu_handoff_provenance.py check"


def sentinel_files(root: Path) -> list[Path]:
    """Every ledger still carrying an unregenerated merge resolution."""

    found = []
    for relative in LEDGERS:
        candidate = root / relative
        try:
            text = candidate.read_text(encoding="utf-8", errors="replace")
        except OSError:
            # A missing ledger is not this guard's business; the provenance
            # suite owns that. Saying nothing is correct, but only because
            # absence cannot hide a sentinel.
            continue
        if SENTINEL in text:
            found.append(relative)
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".")
    parser.add_argument("--mode", choices=("report", "hint"), default="report")
    args = parser.parse_args(argv)

    hits = sentinel_files(Path(args.root).resolve())
    if not hits:
        return 0

    print("", file=sys.stderr)
    print(
        "gpu-ledger-sentinel: a ledger merge was resolved but never regenerated:",
        file=sys.stderr,
    )
    for hit in hits:
        print(f"    {hit}", file=sys.stderr)
    print(
        f"  Every pinned identity there reads {SENTINEL!r}. The merge driver writes",
        file=sys.stderr,
    )
    print(
        "  that on purpose: a plausible-looking merged pin would be committed in",
        file=sys.stderr,
    )
    print("  silence, and this cannot be.", file=sys.stderr)
    print("  Regenerate, and land the result as its own commit — amending a commit", file=sys.stderr)
    print("  that touches a pinned path re-stales the row it just repaired.", file=sys.stderr)
    print(f"  Repair:  {REPAIR}", file=sys.stderr)
    print(f"  Verify:  {VERIFY}", file=sys.stderr)
    return 0 if args.mode == "hint" else 1


if __name__ == "__main__":
    raise SystemExit(main())
