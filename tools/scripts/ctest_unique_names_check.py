#!/usr/bin/env python3
"""Every ctest entry in the build must have a unique name.

A ctest name is the join key for everything downstream of a run: the JUnit
report, the fast-tier contract, the receipt that lets a merge group reuse a
PR head's validation. `protected_merge_receipt.py issue` refuses an inventory
that lists a name twice ("CTest selection lists a test twice"), and because
that step is continue-on-error, a duplicate silently turned receipt reuse
off for every PR head until someone read the step log: 21 duplicated names
(one source compiled into two suites, and same-named Catch2 cases in pairs of
files) meant 0 receipts issued and 0 of 42 merge groups reused.

Usage (as a ctest, against the configured build):
    ctest_unique_names_check.py --build-dir <dir> [--ctest <ctest>]
Or against a saved `ctest --show-only=json-v1` document:
    ctest_unique_names_check.py --show-only-json <file>

Exit 0 when every name is unique; 1 listing each duplicate with the commands
that produced it; 2 when the inventory cannot be read (never a pass: an empty
inventory means the instrument is pointed at the wrong directory).
"""
from __future__ import annotations

import argparse
import collections
import json
import subprocess
import sys
from pathlib import Path


def duplicates(document: dict) -> dict[str, list[str]]:
    """name → the distinct command lines registered under it, for names seen
    more than once."""
    by_name: dict[str, list[str]] = collections.defaultdict(list)
    for test in document.get("tests", []):
        name = test.get("name")
        if name is None:
            continue
        by_name[name].append(" ".join(test.get("command") or ["<no command>"]))
    return {n: sorted(set(c)) for n, c in by_name.items() if len(c) > 1}


def load_inventory(args: argparse.Namespace) -> dict | None:
    if args.show_only_json:
        try:
            return json.loads(Path(args.show_only_json).read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            print(f"ERROR: could not read inventory: {exc}", file=sys.stderr)
            return None
    proc = subprocess.run([args.ctest, "--test-dir", args.build_dir, "-N", "--show-only=json-v1"],
                          capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        print(f"ERROR: ctest -N failed ({proc.returncode}): {proc.stderr.strip()}", file=sys.stderr)
        return None
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        print(f"ERROR: ctest --show-only=json-v1 was not JSON: {exc}", file=sys.stderr)
        return None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir")
    ap.add_argument("--ctest", default="ctest")
    ap.add_argument("--show-only-json")
    args = ap.parse_args(argv[1:])
    if not args.show_only_json and not args.build_dir:
        ap.error("--build-dir or --show-only-json is required")
    document = load_inventory(args)
    if document is None:
        return 2
    total = len(document.get("tests", []))
    if total == 0:
        # Control: an empty listing is the instrument, not a clean suite.
        print("ERROR: the inventory lists no tests; wrong build directory?", file=sys.stderr)
        return 2
    dups = duplicates(document)
    if not dups:
        print(f"OK: {total} ctest entries, all names unique")
        return 0
    print(f"ERROR: {len(dups)} ctest name(s) registered more than once (of {total} entries):")
    for name in sorted(dups):
        print(f"  {name!r}")
        for cmd in dups[name]:
            print(f"      {cmd}")
    print("Rename the Catch2 case or register the source in one suite only; "
          "protected_merge_receipt.py refuses duplicate names.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
