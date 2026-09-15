#!/usr/bin/env python3
"""Three-way merge for the generated GPU handoff ledger and its receipt.

``docs/status/gpu-vellum-handoff.yaml`` and
``docs/validation/gpu-handoff-provenance/receipt.json`` are regenerated from one
exact commit by ``gpu_handoff_provenance.py``. Every re-pin rewrites an identity
on roughly a hundred rows, so any branch that outlives a main move collides on
both files even when nobody edited them: the collision is churn, not
disagreement.

The churn and the content live in the same file, which is what makes this
delicate. Resolving the whole file to one side would take the churn *and* throw
away the other side's rows -- and the loss is silent, because regeneration only
rewrites the identity fields of rows that already exist, and the always-on
provenance tier only checks that the rows still present route correctly. A
dropped row is a document that validates.

So this normalizes rather than chooses. Every field the generator rewrites is
replaced, in all three inputs, by a value no tier accepts, which makes the churn
byte-identical on both sides and leaves an ordinary three-way merge to do its
job on everything else. What survives is the union: a row one side added is a
one-sided change and merges cleanly, while two sides editing the same row is a
real disagreement and still comes back as conflict markers and a nonzero exit.

The invalid value is deliberate. Every value that merely *looks* like an
identity is accepted somewhere -- a stale-but-ancestral pin satisfies the
provenance tier -- so a driver that computed the merged pin would hand Git a
wrong answer it would commit without a word. ``regenerate-me`` fails the 40-hex
check in ``gpu_recipe_catalog.validate_handoff``, fails the blob comparison in
``validate_handoff_routing``, and is rejected by ``gpu_ledger_sentinel_check.py``
from the pre-push hook before it can reach CI. The merge stops being a
hand-resolved conflict and becomes one mechanical regeneration.

Poison only what ``write`` can regenerate, and only where it can regenerate it:

* ``pulp_paths`` rows carry identities derived from this repository's history.
  ``vellum_paths`` rows are constants pinned to a fixed foreign revision and
  validated against a hardcoded table, so the generator never rewrites them --
  poisoning one produces damage no repair command can undo. The two are told
  apart by reading the parsed ``pulp_paths`` array, not by tracking key order
  through the file.
* The receipt's ``source_commit`` and ``handoff_sha256`` change on every re-pin.
  Its ``canonical_paths`` list and the count and digest over it are left to
  merge: they change only when a row set changes, so they are one-sided exactly
  when a row addition is one-sided, and genuinely conflicting when two authors
  add rows at once.

Git invokes this from the top of the working tree for merge, rebase,
cherry-pick and stash alike. ``%O`` is base, ``%A`` is ours and also the output
file, ``%B`` is theirs.
"""
from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile

# Must equal the value gpu_ledger_sentinel_check.py scans for. Git runs this as
# a bare command mid-merge with no import path to rely on, so the two literals
# are kept in step by a test rather than by construction.
SENTINEL = "regenerate-me"

# What `gpu_handoff_provenance.py write` rewrites on a Pulp row. `object_type`
# is also regenerable but is a per-row constant that only moves if a pinned path
# changes kind, so it is left to merge: a change in it is content worth seeing.
LEDGER_IDENTITY_FIELDS = ("revision", "object_id")
RECEIPT_REGENERABLE_FIELDS = ("source_commit", "handoff_sha256")


def poison(document: object) -> object:
    """Replace every regenerable field with the sentinel, in place."""

    if not isinstance(document, dict):
        return document
    for entry in document.get("entries", []) or []:
        if not isinstance(entry, dict):
            continue
        for row in entry.get("pulp_paths", []) or []:
            if not isinstance(row, dict):
                continue
            for field in LEDGER_IDENTITY_FIELDS:
                if field in row:
                    row[field] = SENTINEL
    if "handoff_sha256" in document:
        for field in RECEIPT_REGENERABLE_FIELDS:
            if field in document:
                document[field] = SENTINEL
    return document


def normalized(raw: str) -> str:
    """Parse, poison, and re-emit in the generator's deterministic encoding.

    Re-emitting canonically is what lets the three-way merge compare like with
    like: a side whose whitespace drifted would otherwise read as an edit on
    every line it touched. `write` re-canonicalizes the file anyway, so nothing
    a human would want to keep is expressed in the formatting.
    """

    return (
        json.dumps(
            poison(json.loads(raw)),
            indent=2,
            ensure_ascii=True,
            separators=(",", ": "),
        )
        + "\n"
    )


def merge_file(ours: pathlib.Path, base: pathlib.Path, theirs: pathlib.Path) -> tuple[int, str]:
    """Three-way merge to stdout, so a failure cannot truncate the real file."""

    completed = subprocess.run(
        [
            "git", "merge-file", "-p",
            "-L", "ours", "-L", "base", "-L", "theirs",
            str(ours), str(base), str(theirs),
        ],
        capture_output=True, text=True, check=False,
    )
    return completed.returncode, completed.stdout


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(
            "gpu-ledger-merge-driver: expected %O %A %B "
            f"(base, ours/output, theirs); got {len(argv)} argument(s). "
            "Re-run tools/scripts/install-githooks.sh to refresh the "
            "registration.",
            file=sys.stderr,
        )
        return 1
    base, ours, theirs = (pathlib.Path(path) for path in argv)

    try:
        sources = {path: path.read_text(encoding="utf-8") for path in (base, ours, theirs)}
        normal = {path: normalized(raw) for path, raw in sources.items()}
    except (OSError, ValueError):
        # Unreadable, empty, or not JSON -- an add/add with no base, or a file
        # somebody already left conflicted. Merge the bytes as they are and let
        # the result be as loud as it needs to be; normalizing is an
        # optimization for the churn, never a precondition for correctness.
        status, merged = merge_file(ours, base, theirs)
        if 0 <= status < 128:
            ours.write_text(merged, encoding="utf-8")
        return 0 if status == 0 else 1

    with tempfile.TemporaryDirectory() as directory:
        scratch = pathlib.Path(directory)
        staged = {}
        for name, path in (("base", base), ("ours", ours), ("theirs", theirs)):
            staged[name] = scratch / name
            staged[name].write_text(normal[path], encoding="utf-8")
        status, merged = merge_file(staged["ours"], staged["base"], staged["theirs"])

    if status < 0 or status >= 128:
        # merge-file could not run. Leave ours untouched rather than overwrite
        # the file with whatever landed on stdout, and report a conflict so the
        # collision reaches a human instead of being resolved to a guess.
        print(
            "gpu-ledger-merge-driver: git merge-file failed; leaving the "
            "conflict for manual resolution.",
            file=sys.stderr,
        )
        return 1
    ours.write_text(merged, encoding="utf-8")
    return 0 if status == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
