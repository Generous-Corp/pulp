#!/usr/bin/env python3
"""A PR must not re-pin the GPU/Vellum ledger; the version bot does that now.

``docs/status/gpu-vellum-handoff.yaml`` pins every referenced Pulp path to a
revision, object id and object type, and
``docs/validation/gpu-handoff-provenance/receipt.json`` binds itself to that
ledger's exact bytes. Both are generated, and both go stale the instant any
pinned path changes — which used to make every such PR carry a hand-run
re-pin as a second commit.

That cost more than it looked like. Measured over 25 days: ~148 non-merge
commits changed **only** those two files, in 124 distinct subject spellings.
Worse, they collide on github.com, where the local ``pulp-gpu-ledger`` merge
driver cannot run — so two PRs that both re-pinned went ``DIRTY`` against each
other and serialized on a generated artifact neither author had opinions about.

``version_at_land._refresh_derived`` now regenerates the pair inside the bump
commit the release bot already writes to ``main``. That commit is the one place
a re-pin costs nobody a rebase, because it advances ``main`` by itself. So the
rule inverted: **an ordinary PR does not re-pin.**

This guard therefore no longer asks "did you forget the refresh?". It asks the
two questions that are still live:

1. **An identity-only re-pin in a PR.** The ledger or the receipt moved, but
   the *inventory* — which paths are pinned, in which repo, in which state —
   did not. That is precisely the work the bot now does, and doing it in a PR
   re-creates the collision class on a file the bot is about to rewrite anyway.

2. **A pinned path deleted or renamed away while the inventory still lists
   it.** Staleness is survivable — a stale pin is still ancestral, so the
   always-on provenance tier accepts it and currency is opt-in. A *missing*
   path is not: the ledger names a path that no longer exists, and the
   required gate goes red. This is the one remaining shape a PR author must
   fix in the PR, because the bot's regenerator cannot invent an inventory
   decision about a path somebody deliberately removed.

An inventory change is still a legitimate in-PR ledger edit, and it still owes
``gpu_handoff_provenance.py write --receipt`` so the identities and the receipt
binding match the new row set.

Diff-scoped and sub-second by construction: two ``git show`` reads of the
ledger and one ``git diff --name-status``. It does not re-verify identity
fields — that is ``gpu_handoff_provenance.py check``, which costs ~25s because
it runs a ``git log`` per pinned path.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

HANDOFF = Path("docs/status/gpu-vellum-handoff.yaml")
RECEIPT = Path("docs/validation/gpu-handoff-provenance/receipt.json")
# --receipt is not optional in practice: the published receipt binds itself to
# the ledger's exact bytes, so regenerating one without the other leaves the
# receipt claiming a source that no longer exists.
REPAIR = "python3 tools/scripts/gpu_handoff_provenance.py write --receipt"
VERIFY = "python3 tools/scripts/gpu_handoff_provenance.py check"


def _git(root: Path, *args: str) -> tuple[int, str]:
    proc = subprocess.run(
        ["git", *args], cwd=root, capture_output=True, text=True, check=False,
    )
    return proc.returncode, proc.stdout


def _inventory_from_text(text: str) -> set[tuple[str, str, str]] | None:
    """The set of ``(repo, path, state)`` rows the ledger declares.

    This is the ledger's *editorial* content — which paths are pinned at all —
    as opposed to the identity fields (``revision`` / ``object_id`` /
    ``object_type``) a regenerator derives from the tree. Returns ``None`` when
    the document cannot be read, so callers can decline to judge rather than
    report an empty inventory as a real one.

    The file carries a ``.yaml`` extension but its contents are JSON, so it
    parses with the stdlib and needs no PyYAML — which matters because the repo
    already treats a PyYAML dependency as real friction on PEP-668 Python.
    """
    try:
        data = json.loads(text)
    except (TypeError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    rows: set[tuple[str, str, str]] = set()
    for entry in data.get("entries") or []:
        if not isinstance(entry, dict):
            continue
        for row in entry.get("pulp_paths") or []:
            if not isinstance(row, dict):
                continue
            path = row.get("path")
            if isinstance(path, str) and path:
                rows.add((
                    str(row.get("repo") or ""),
                    path,
                    str(row.get("state") or ""),
                ))
    return rows


def _inventory_at(root: Path, rev: str) -> set[tuple[str, str, str]] | None:
    code, out = _git(root, "show", f"{rev}:{HANDOFF}")
    if code != 0:
        return None
    return _inventory_from_text(out)


def pinned_paths(root: Path, rev: str | None = None) -> set[str]:
    """Every ``entries[*].pulp_paths[*].path``, from ``rev`` or the worktree."""
    if rev is None:
        doc = root / HANDOFF
        if not doc.is_file():
            return set()
        try:
            rows = _inventory_from_text(doc.read_text())
        except OSError:
            return set()
    else:
        rows = _inventory_at(root, rev)
    return {path for _, path, _ in rows or ()}


def name_status(base: str, root: Path) -> list[tuple[str, str]]:
    """``(status, path)`` for the range, with a rename reported at its source.

    A rename is the shape that matters here: the ledger pins the old path, and
    a rename removes it just as surely as a delete does.
    """
    code, out = _git(root, "diff", "--name-status", "-M", f"{base}...HEAD")
    if code != 0:
        return []
    rows: list[tuple[str, str]] = []
    for line in out.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        rows.append((parts[0].strip(), parts[1].strip()))
    return rows


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="origin/main")
    ap.add_argument("--mode", choices=("report", "hint"), default="report")
    ap.add_argument("--root", default=".")
    args = ap.parse_args(argv)
    root = Path(args.root).resolve()

    before = _inventory_at(root, args.base)
    after = _inventory_at(root, "HEAD")
    if before is None or after is None:
        # No ledger on one side of the range, or a shape this guard cannot
        # read. Say so rather than passing silently, because a guard that
        # cannot see its subject must never look like one that checked it.
        print("gpu-handoff-pin: ledger unreadable on one side of the range; "
              "nothing checked", file=sys.stderr)
        return 0

    rows = name_status(args.base, root)
    touched = {path for _, path in rows}
    inventory_changed = before != after
    failed = False

    # (i) An identity-only re-pin. The generated bytes moved but the row set
    #     did not, which is exactly the work the bump commit now carries.
    repinned = sorted({str(p) for p in (HANDOFF, RECEIPT)} & touched)
    if repinned and not inventory_changed:
        failed = True
        print("", file=sys.stderr)
        print("gpu-handoff-pin: this PR re-pins the generated ledger:", file=sys.stderr)
        for path in repinned:
            print(f"    {path}", file=sys.stderr)
        print("  The pinned paths themselves are unchanged, so this is an identity-only", file=sys.stderr)
        print("  refresh — which the version bot's bump commit now performs on main", file=sys.stderr)
        print("  (version_at_land._refresh_derived). Doing it here re-creates the", file=sys.stderr)
        print("  collision this moved: the pulp-gpu-ledger merge driver cannot run on", file=sys.stderr)
        print("  github.com, so two PRs that both re-pin go DIRTY against each other.", file=sys.stderr)
        print("  Drop the re-pin commit:", file=sys.stderr)
        print(f"      git checkout {args.base} -- {HANDOFF} {RECEIPT}", file=sys.stderr)
        print("  A ledger edit is still expected when you change the INVENTORY (adding,", file=sys.stderr)
        print("  removing or re-stating a pinned path). That edit does owe the", file=sys.stderr)
        print(f"  regeneration:  {REPAIR}", file=sys.stderr)

    # (ii) A pinned path removed while the inventory still lists it. Unlike
    #      staleness, this is not survivable: the row names a path that is not
    #      there, and the required gate goes red.
    gone = sorted({
        path for status, path in rows
        if status.startswith(("D", "R"))
    } & {path for _, path, _ in after})
    if gone:
        failed = True
        print("", file=sys.stderr)
        print("gpu-handoff-pin: pinned path(s) deleted or renamed, but still listed", file=sys.stderr)
        print("in the ledger inventory:", file=sys.stderr)
        for path in gone:
            print(f"    {path}", file=sys.stderr)
        print("  A stale pin survives (provenance is ancestral, currency is opt-in);", file=sys.stderr)
        print("  a MISSING one does not — the required gate goes red on it, and the", file=sys.stderr)
        print("  bot cannot guess whether you meant to drop the row or move it.", file=sys.stderr)
        print("  Update the inventory in this PR, then regenerate:", file=sys.stderr)
        print(f"      {REPAIR}", file=sys.stderr)
        print(f"      {VERIFY}", file=sys.stderr)

    if not failed:
        return 0
    return 0 if args.mode == "hint" else 1


if __name__ == "__main__":
    raise SystemExit(main())
