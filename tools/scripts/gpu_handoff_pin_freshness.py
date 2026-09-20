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
   every byte of the ledger a human writes stayed put — only the three derived
   identity fields moved. That is precisely the work the bot now does, and
   doing it in a PR re-creates the collision class on a file the bot is about
   to rewrite anyway.

2. **A pinned path deleted or renamed away while the inventory still lists
   it.** Staleness is survivable — a stale pin is still ancestral, so the
   always-on provenance tier accepts it and currency is opt-in. A *missing*
   path is not: the ledger names a path that no longer exists, and the
   required gate goes red. This is the one remaining shape a PR author must
   fix in the PR, because the bot's regenerator cannot invent an inventory
   decision about a path somebody deliberately removed.

Any **editorial** ledger edit is still a legitimate in-PR change, and it still
owes ``gpu_handoff_provenance.py write --receipt`` so the identities and the
receipt binding match the edited document. Editorial is deliberately the whole
document minus the three derived fields, not just the pinned row set: the
ledger also carries ``authorities``, ``upstream``, ``cutover_trigger``,
``self_binding``, ``stop_rules`` and per-entry ``vellum_paths``,
``terminal_evidence``, ``input_receipts`` and ``accepted_dispositions``, none
of which a regenerator writes. Judging by the row set alone would reject those
edits as re-pins: across 25 days of ``main``, seven ledger commits moved an
editorial field while leaving the row set untouched.

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
# The three fields `gpu_handoff_provenance.py write` derives from the tree.
# Everything else in the document is editorial, and a PR may change it freely.
IDENTITY_FIELDS = ("revision", "object_id", "object_type")
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


def _inventory_from_text(text: str) -> str | None:
    """The ledger's editorial content, normalised for comparison.

    Editorial means everything a regenerator does not write: the whole
    document minus the three identity fields (``revision``, ``object_id``,
    ``object_type``) that ``gpu_handoff_provenance.py write`` derives from the
    tree. Keyed that way, two ledgers compare equal exactly when a human
    changed nothing and only a re-pin moved — which is the question this guard
    asks.

    The narrower key it replaced — the set of ``(repo, path, state)`` rows —
    was wrong in a shape that happens: the ledger also carries top-level
    ``authorities`` / ``upstream`` / ``cutover_trigger`` / ``self_binding`` /
    ``stop_rules`` and per-entry ``vellum_paths`` / ``terminal_evidence`` /
    ``input_receipts`` / ``accepted_dispositions``. Editing any of those leaves
    the row set identical, so the row-set key read a real editorial commit as
    an identity-only re-pin and rejected it.

    Returns ``None`` when the document cannot be read, so callers can decline
    to judge rather than report an empty inventory as a real one.

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
    for entry in data.get("entries") or []:
        if not isinstance(entry, dict):
            continue
        for row in entry.get("pulp_paths") or []:
            if not isinstance(row, dict):
                continue
            for field in IDENTITY_FIELDS:
                row.pop(field, None)
    # Sorted keys so a pure key-order change in the generator's output cannot
    # masquerade as an editorial edit, and a compact separator so whitespace
    # cannot either.
    return json.dumps(data, sort_keys=True, separators=(",", ":"))


def _pinned_paths_from_text(text: str) -> set[str] | None:
    """Every ``entries[*].pulp_paths[*].path`` the ledger declares.

    Separate from the editorial form above because the two answer different
    questions: this one is the list of paths whose disappearance turns the
    required gate red, and it must stay a set of paths rather than a document.
    """
    try:
        data = json.loads(text)
    except (TypeError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    paths: set[str] = set()
    for entry in data.get("entries") or []:
        if not isinstance(entry, dict):
            continue
        for row in entry.get("pulp_paths") or []:
            if not isinstance(row, dict):
                continue
            path = row.get("path")
            if isinstance(path, str) and path:
                paths.add(path)
    return paths


def _ledger_text_at(root: Path, rev: str) -> str | None:
    code, out = _git(root, "show", f"{rev}:{HANDOFF}")
    return out if code == 0 else None


def pinned_paths(root: Path, rev: str | None = None) -> set[str]:
    """Every ``entries[*].pulp_paths[*].path``, from ``rev`` or the worktree."""
    if rev is None:
        doc = root / HANDOFF
        if not doc.is_file():
            return set()
        try:
            text = doc.read_text()
        except OSError:
            return set()
    else:
        text = _ledger_text_at(root, rev)
        if text is None:
            return set()
    return _pinned_paths_from_text(text) or set()


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

    before_text = _ledger_text_at(root, args.base)
    after_text = _ledger_text_at(root, "HEAD")
    before = _inventory_from_text(before_text) if before_text is not None else None
    after = _inventory_from_text(after_text) if after_text is not None else None
    if before is None or after is None:
        # No ledger on one side of the range, or a shape this guard cannot
        # read. Say so rather than passing silently, because a guard that
        # cannot see its subject must never look like one that checked it.
        print("gpu-handoff-pin: ledger unreadable on one side of the range; "
              "nothing checked", file=sys.stderr)
        return 0

    rows = name_status(args.base, root)
    touched = {path for _, path in rows}
    editorial_changed = before != after
    failed = False

    # (i) An identity-only re-pin. The generated bytes moved but the ledger's
    #     editorial content did not, which is exactly the work the bump commit
    #     now carries.
    repinned = sorted({str(p) for p in (HANDOFF, RECEIPT)} & touched)
    if repinned and not editorial_changed:
        failed = True
        print("", file=sys.stderr)
        print("gpu-handoff-pin: this PR re-pins the generated ledger:", file=sys.stderr)
        for path in repinned:
            print(f"    {path}", file=sys.stderr)
        print("  Nothing a human writes in that ledger changed — only the derived", file=sys.stderr)
        print(f"  identity fields ({', '.join(IDENTITY_FIELDS)}) — so this is an", file=sys.stderr)
        print("  identity-only refresh, which the version bot's bump commit now", file=sys.stderr)
        print("  performs on main", file=sys.stderr)
        print("  (version_at_land._refresh_derived). Doing it here re-creates the", file=sys.stderr)
        print("  collision this moved: the pulp-gpu-ledger merge driver cannot run on", file=sys.stderr)
        print("  github.com, so two PRs that both re-pin go DIRTY against each other.", file=sys.stderr)
        print("  Drop the re-pin commit:", file=sys.stderr)
        print(f"      git checkout {args.base} -- {HANDOFF} {RECEIPT}", file=sys.stderr)
        print("  An EDITORIAL ledger edit is still expected and still passes: adding,", file=sys.stderr)
        print("  removing or re-stating a pinned row, and equally a change to", file=sys.stderr)
        print("  authorities, upstream, cutover_trigger, self_binding, stop_rules,", file=sys.stderr)
        print("  vellum_paths, terminal_evidence, input_receipts or", file=sys.stderr)
        print("  accepted_dispositions. That edit does owe the regeneration:", file=sys.stderr)
        print(f"      {REPAIR}", file=sys.stderr)

    # (ii) A pinned path removed while the inventory still lists it. Unlike
    #      staleness, this is not survivable: the row names a path that is not
    #      there, and the required gate goes red.
    gone = sorted({
        path for status, path in rows
        if status.startswith(("D", "R"))
    } & (_pinned_paths_from_text(after_text) or set()))
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
