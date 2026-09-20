#!/usr/bin/env python3
"""Chasing pin freshness on a feature branch is churn; reject it at push.

``docs/status/gpu-vellum-handoff.yaml`` and its receipt pin every referenced
Pulp path to a revision, object id and object type. This guard used to demand
the opposite of what it demands now: it failed a branch that edited a pinned
path *without* regenerating. That polarity was the right one while the cost of a
stale pin looked like a red required gate. It is the wrong one now, and the
measurement says so.

Regeneration is not free, and the price is not paid by the branch that pays it.
``source_commit`` and the ``handoff_sha256`` computed over the whole ledger move
on *every* regeneration, whatever else did or did not change, so two branches
that both regenerate collide on the receipt unconditionally. Neither GitHub's
mergeability check nor the merge queue can reach the ``pulp-gpu-ledger`` merge
driver -- both do a plain text merge -- so the collision is a real conflict, and
merging one ledger-carrying pull request re-conflicts every other one. Roughly
one lands per refresh round.

The rows themselves are not the problem, which is the part that is easy to get
backwards. A row's revision is ``git log -1 C -- path``, so a main move re-pins
only the rows whose paths that move touched -- a median of two, and never more
than nine, over the 52 ledger-carrying merges on main as of a15a7ff4455, with
46% of them moving exactly one row. The unconditional collision is the receipt:
``source_commit`` and ``handoff_sha256`` move for no reason but the
regeneration itself.

What the refresh buys is *currency* -- the claim that a pin still describes
HEAD. That tier is opt-in behind ``PULP_GPU_HANDOFF_REQUIRE_CURRENT``, which
nothing in ``.github`` sets. What it does not buy is provenance or coherence,
and those hold with or without it: a pin that has merely fallen behind is still
an ancestor of HEAD and still resolves ``revision:path`` to exactly the object
id it recorded, so ``validate_handoff_routing``'s always-on tier stays green.
The branch pays a guaranteed conflict for an opt-in property.

So the guard rejects the churn instead of demanding it. What it must not do is
reject the *content*, because the ledger is a hybrid and not a generated
artefact: ``gpu_handoff_provenance.py`` says so itself -- "the generator never
adds, removes, or reorders rows; a path list change is a human edit". The 185
commits that touched the ledger as of a15a7ff4455 decompose as one creation
that pinned no rows at all, six human edits that built the inventory to 102
rows, and 178 identity re-pins that moved no path. Only the ratio decays --
re-measured at e17c9cd164 it is 184 re-pins against the same one creation and
the same six edits. A blanket "do not touch these files" would outlaw the six
to stop the rest, and it would also strand a real
correction: ``gpu_recipe_catalog.validate_handoff_routing`` checks
``expansion_id`` and ``route_set_sha256`` against the live ownership projection
unconditionally, and neither is regenerable, so a branch that had to fix one
would be red on the required gate and forbidden from fixing it.

``sequencer_exposure_check.py`` faced this same fork and recorded the answer:
"Excluding one WHOLE would drop real coverage ... So the exemption is scoped to
the transition instead of the path." This is that shape. A diff confined to the
fields the generator rewrites is mechanical and is rejected; any other edit to
the same files is content, and stays permitted.

Scope is the whole change set, not each file: the receipt binds itself to the
ledger's exact bytes, so a genuine inventory edit necessarily drags
``handoff_sha256`` along with it. Only when *every* watched file the range
touched is churn-only is the range churn.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

# The merge driver already owns the authoritative answer to "what does the
# generator rewrite?", and it owns it as a walk over the document rather than as
# a field list, because a `vellum_paths` row carries the same field names and is
# never regenerated. Importing it is what keeps the guard and the driver from
# drifting into two different definitions of churn: the driver neutralizes
# exactly these fields to make a merge succeed, and the guard neutralizes
# exactly these fields to decide a diff is empty. Same question, one answer.
import gpu_ledger_merge_driver as driver  # noqa: E402

HANDOFF = Path("docs/status/gpu-vellum-handoff.yaml")
RECEIPT = Path("docs/validation/gpu-handoff-provenance/receipt.json")
WATCHED = (HANDOFF, RECEIPT)

# Named in the failure text so the reader can see what the guard does not
# object to, rather than inferring a blanket ban from a single rejection.
CONTENT_FIELDS = (
    "a pinned path added to or removed from entries[*].pulp_paths",
    "route_set_sha256 / expansion_id (the ownership projection moved)",
    "upstream.*, authorities.*, and every other declarative field",
)


def git(root: Path, *arguments: str) -> str | None:
    """stdout of a successful git command, or None when it failed."""

    completed = subprocess.run(
        ["git", "-C", str(root), *arguments],
        capture_output=True, text=True, check=False,
    )
    return completed.stdout if completed.returncode == 0 else None


def changed_paths(root: Path, base: str) -> set[str] | None:
    """Every path the range touched, or None when the range cannot be read.

    None rather than an empty set: an unresolvable base ref produces the same
    empty answer as a branch that changed nothing, and the caller must be able
    to say which it got instead of reporting a pass it never established.
    """

    out = git(root, "diff", "--name-only", f"{base}...HEAD")
    if out is None:
        return None
    return {line.strip() for line in out.splitlines() if line.strip()}


def neutralized(raw: str) -> object | None:
    """The document with every regenerable field replaced, or None if unreadable.

    Comparing two neutralized documents answers "is anything here but churn?"
    without enumerating fields: a row present on one side only survives
    neutralization and still compares unequal, which is why an added row reads
    as content while a re-pinned one does not.
    """

    try:
        return driver.poison(json.loads(raw))
    except (TypeError, ValueError):
        # TypeError as well as ValueError: `poison` walks `entries` and
        # `pulp_paths` with `or []`, which turns None into an empty list but
        # leaves a scalar to raise on iteration. A malformed ledger is the
        # provenance suite's business, not this guard's, but it must not become
        # a traceback here either.
        return None


def churn_only(root: Path, before_rev: str, path: Path) -> bool | None:
    """Whether this file's change in the range rewrites nothing but identities.

    None means "could not tell" -- the file was added or removed outright, or
    either side does not parse. A guard that cannot see its subject must not
    look like one that checked it, so the caller reports rather than decides.
    """

    before = git(root, "show", f"{before_rev}:{path.as_posix()}")
    after = git(root, "show", f"HEAD:{path.as_posix()}")
    if before is None or after is None:
        return None
    left, right = neutralized(before), neutralized(after)
    if left is None or right is None:
        return None
    return left == right


def report(base: str, churned: list[Path]) -> None:
    print("", file=sys.stderr)
    print("gpu-handoff-churn: this range regenerates the GPU handoff ledger's",
          file=sys.stderr)
    print("  generated identities and changes nothing else:", file=sys.stderr)
    for path in churned:
        print(f"    {path}", file=sys.stderr)
    print("  The receipt's source_commit and handoff_sha256 move on every", file=sys.stderr)
    print("  regeneration whatever else changed, and neither GitHub's mergeability", file=sys.stderr)
    print("  check nor the merge queue can run the pulp-gpu-ledger merge driver —", file=sys.stderr)
    print("  both text-merge — so this refresh conflicts with every other branch", file=sys.stderr)
    print("  carrying one, and each merge re-conflicts the rest.", file=sys.stderr)
    print("  Nothing downstream is waiting for it: currency is opt-in behind", file=sys.stderr)
    print("  PULP_GPU_HANDOFF_REQUIRE_CURRENT, which nothing sets, and a pin that", file=sys.stderr)
    print("  has merely fallen behind still satisfies the always-on provenance", file=sys.stderr)
    print("  tier. Main carries the refresh; this branch does not need to.", file=sys.stderr)
    print("  Drop it:", file=sys.stderr)
    print(f"    git restore --source=$(git merge-base {base} HEAD) -- \\", file=sys.stderr)
    print(f"        {HANDOFF} \\", file=sys.stderr)
    print(f"        {RECEIPT}", file=sys.stderr)
    print("    git commit -m 'drop generated ledger churn'   # or amend", file=sys.stderr)
    print("  This guard objects to identity churn only. Still permitted, and still", file=sys.stderr)
    print("  requiring the receipt regenerated alongside them:", file=sys.stderr)
    for description in CONTENT_FIELDS:
        print(f"    {description}", file=sys.stderr)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--mode", choices=("report", "hint"), default="report")
    parser.add_argument("--root", default=".")
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()

    touched = changed_paths(root, args.base)
    if touched is None:
        print(f"gpu-handoff-churn: cannot diff {args.base}...HEAD; nothing checked",
              file=sys.stderr)
        return 0
    watched = [path for path in WATCHED if path.as_posix() in touched]
    if not watched:
        return 0

    # The range is a three-dot diff, so the "before" side is the merge base and
    # not the base ref. Reading the base ref instead would attribute main's own
    # refreshes to this branch, which is the one false positive that would make
    # the guard unusable on a long-lived branch.
    merge_base = git(root, "merge-base", args.base, "HEAD")
    before_rev = merge_base.strip() if merge_base and merge_base.strip() else args.base

    verdicts = {path: churn_only(root, before_rev, path) for path in watched}
    if any(verdict is None for verdict in verdicts.values()):
        print("gpu-handoff-churn: a watched ledger was added, removed, or does not "
              "parse; nothing checked", file=sys.stderr)
        return 0
    if not all(verdicts.values()):
        return 0

    report(args.base, watched)
    return 0 if args.mode == "hint" else 1


if __name__ == "__main__":
    raise SystemExit(main())
