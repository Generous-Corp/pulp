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

Diff-scoped and sub-second by construction: one ``ls-tree``/``show`` pair per
side of the range and one ``git diff --name-status``. It does not re-verify
identity fields — that is ``gpu_handoff_provenance.py check``, which costs ~25s
because it runs a ``git log`` per pinned path.

**A guard that cannot see its subject must never look like one that checked
it.** Every git read here is checked, and a failure is reported as its own
outcome rather than degraded into an empty answer. An empty changed-path list
means "nothing changed", so a swallowed ``git diff`` failure — an unresolvable
base, a deleted branch, a shallow clone, a typo — would exit clean while the
violation it exists to catch sailed through. Absence is still an answer where
it genuinely is one: a ledger that simply post-dates one side of the range is
nothing to compare, not a broken instrument.

Exit codes:
    0 — checked and clean, or nothing to compare; also every hint-mode run
    1 — a violation was found (report mode)
    2 — git could not answer, so NO VERDICT was produced (report mode)

Two is deliberately not zero. "Could not check" and "checked and clean" are
different findings and must never be spelled the same way.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from gate_common import git_comparison_receipt, resolve_git_comparison

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

# The verdict this gate could not produce, kept distinct from both a clean run
# and a violation so a caller can route it deliberately.
NO_VERDICT = 2

# How a read of the ledger at a revision turned out. `path_absent` is an
# ANSWER — the ledger may simply post-date that revision. `command_failed` is
# the absence of one.
LEDGER_AVAILABLE = "available"
LEDGER_ABSENT = "path_absent"
LEDGER_UNREADABLE = "command_failed"


class LedgerUnavailable(RuntimeError):
    """Git could not answer, so no statement about the ledger is possible.

    Raised instead of returning an empty set, because an empty inventory is
    indistinguishable from a ledger that pins nothing — and the caller would
    read the second meaning.
    """


def _git(root: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args], cwd=root, capture_output=True, text=True, check=False,
    )


def _detail(proc: subprocess.CompletedProcess) -> str:
    return " ".join((proc.stderr or "").strip().split())[:512]


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


def read_ledger_at(root: Path, rev: str) -> tuple[str | None, str, str]:
    """``(text, status, detail)`` for the ledger as of ``rev``.

    ``ls-tree`` first, then ``show``, so a ledger that is genuinely absent at
    ``rev`` is distinguishable from a revision git cannot read at all. Reading
    only ``show`` collapses the two into one non-zero exit, and treating that
    as "no ledger here" is exactly how an unresolvable base passes for clean.
    """
    listed = _git(root, "ls-tree", "-z", rev, "--", str(HANDOFF))
    if listed.returncode != 0:
        return None, LEDGER_UNREADABLE, _detail(listed) or f"git ls-tree {rev} failed"
    if not listed.stdout:
        return None, LEDGER_ABSENT, ""
    shown = _git(root, "show", f"{rev}:{HANDOFF}")
    if shown.returncode != 0:
        return None, LEDGER_UNREADABLE, (
            _detail(shown) or f"git show {rev}:{HANDOFF} failed"
        )
    return shown.stdout, LEDGER_AVAILABLE, ""


def pinned_paths(root: Path, rev: str | None = None) -> set[str]:
    """Every ``entries[*].pulp_paths[*].path``, from ``rev`` or the worktree.

    Raises ``LedgerUnavailable`` when the ledger is there but cannot be read.
    An empty set is returned only when the ledger genuinely does not exist,
    which is a statement about the inventory rather than about the instrument.
    """
    if rev is None:
        doc = root / HANDOFF
        if not doc.is_file():
            return set()
        try:
            text: str | None = doc.read_text()
        except OSError as error:
            raise LedgerUnavailable(f"cannot read {HANDOFF}: {error}") from error
    else:
        text, status, detail = read_ledger_at(root, rev)
        if status == LEDGER_UNREADABLE:
            raise LedgerUnavailable(f"cannot read {HANDOFF} at {rev}: {detail}")
        if status == LEDGER_ABSENT:
            return set()
    rows = _pinned_paths_from_text(text or "")
    if rows is None:
        where = rev or "the worktree"
        raise LedgerUnavailable(f"{HANDOFF} does not parse at {where}")
    return rows


def name_status(base: str, root: Path, head: str = "HEAD") -> tuple[list[tuple[str, str]], str]:
    """``(rows, detail)`` for the range, with a rename reported at its source.

    A rename is the shape that matters here: the ledger pins the old path, and
    a rename removes it just as surely as a delete does.

    A non-empty ``detail`` means git did not answer, and the empty row list
    that accompanies it means nothing. The caller must not read it as "no
    paths changed" — an empty changed-path list is how this gate spells clean.
    """
    proc = _git(root, "diff", "--name-status", "-M", f"{base}...{head}")
    if proc.returncode != 0:
        return [], _detail(proc) or f"git diff {base}...{head} failed"
    rows: list[tuple[str, str]] = []
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        rows.append((parts[0].strip(), parts[1].strip()))
    return rows, ""


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="origin/main")
    ap.add_argument("--mode", choices=("report", "hint"), default="report")
    ap.add_argument("--root", default=".")
    args = ap.parse_args(argv)
    root = Path(args.root).resolve()

    def no_verdict(detail: str, receipt: str = "") -> int:
        print("", file=sys.stderr)
        print("gpu-handoff-pin: COULD NOT CHECK — no verdict produced", file=sys.stderr)
        print(f"    {detail}", file=sys.stderr)
        if receipt:
            print(f"    comparison receipt={receipt}", file=sys.stderr)
        print("  This is NOT a clean result. The guard could not see its subject,", file=sys.stderr)
        print("  so an in-PR re-pin or an orphaned pinned path would be invisible", file=sys.stderr)
        print("  to it either way. Give it a base it can resolve — fetch the ref,", file=sys.stderr)
        print("  unshallow the clone, or correct the spelling — and run it again:", file=sys.stderr)
        print("      python3 tools/scripts/gpu_handoff_pin_freshness.py \\", file=sys.stderr)
        print("          --base <ref> --mode=report", file=sys.stderr)
        return NO_VERDICT if args.mode == "report" else 0

    comparison = resolve_git_comparison(
        root, args.base, "HEAD", source="gpu_handoff_pin_freshness",
    )
    receipt = git_comparison_receipt(comparison)
    if (
        comparison.status != "available"
        or comparison.comparison_anchor is None
        or comparison.resolved_head is None
    ):
        return no_verdict(
            f"git cannot resolve {args.base}...HEAD ({comparison.status}): "
            f"{comparison.stderr or 'no detail reported'}",
            receipt,
        )
    anchor = comparison.comparison_anchor
    head = comparison.resolved_head

    before_text, before_status, before_detail = read_ledger_at(root, anchor)
    if before_status == LEDGER_UNREADABLE:
        return no_verdict(f"cannot read {HANDOFF} at {anchor}: {before_detail}", receipt)
    after_text, after_status, after_detail = read_ledger_at(root, head)
    if after_status == LEDGER_UNREADABLE:
        return no_verdict(f"cannot read {HANDOFF} at {head}: {after_detail}", receipt)
    if before_status == LEDGER_ABSENT or after_status == LEDGER_ABSENT:
        # A real answer, not a failed read: the ledger post-dates one side of
        # the range, so there is no editorial comparison to make.
        print(f"gpu-handoff-pin: no {HANDOFF} on one side of the range; "
              "nothing to compare", file=sys.stderr)
        return 0

    before = _inventory_from_text(before_text or "")
    after = _inventory_from_text(after_text or "")
    if before is None or after is None:
        return no_verdict(
            f"{HANDOFF} is present but does not parse on one side of the "
            "range, so its editorial content cannot be compared",
            receipt,
        )

    rows, diff_detail = name_status(anchor, root, head)
    if diff_detail:
        return no_verdict(f"cannot diff {anchor}..{head}: {diff_detail}", receipt)

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
    } & (_pinned_paths_from_text(after_text or "") or set()))
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
        print("gpu-handoff-pin: checked, clean — no in-PR re-pin, no orphaned "
              "pinned path", file=sys.stderr)
        return 0
    return 0 if args.mode == "hint" else 1


if __name__ == "__main__":
    raise SystemExit(main())
