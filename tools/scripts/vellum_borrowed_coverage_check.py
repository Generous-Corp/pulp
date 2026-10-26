#!/usr/bin/env python3
"""Fail a pull request whose Vellum watch coverage is borrowed from a stacked parent.

`Vellum freeze` measures a pull request as live `main` merged with its head.
On a branch stacked on another unmerged pull request, that range also holds
the parent's commits, including the parent's watch event, so the check passes
while the branch's own change has no event. Once the parent merges, the merge
queue measures only this branch's change, finds no event, and ejects the whole
batch. This check runs in the pull_request job only, after the watch check has
passed, and fails exactly when every watch event in the range was added by a
commit another remote branch carries, and the branch's own commits beyond
that branch affect a watched family with no event of their own
(`vellum_watch_preflight.borrowed_watch_coverage`, the pre-push advisory's
rule). An event of the branch's own, no event in the range, or only unwatched
commits past the carrier (a parent's review fix, a follow-up to a stale copy)
passes. The merge_group job never runs
it: there the queued base already holds every merged parent.

    vellum_borrowed_coverage_check.py --base <live main> --head <candidate>
        --source-head <PR head sha> --own-branch <PR head ref>

Exit status: 0 pass, 1 borrowed coverage.
"""

from __future__ import annotations

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import vellum_watch_preflight as preflight  # noqa: E402


def _describe(carrier: str) -> str:
    branch = carrier.split("/", 1)[1] if "/" in carrier else carrier
    return f"the pull request from {branch} ({carrier})"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--base", required=True, help="live main the candidate merges into")
    parser.add_argument("--head", required=True, help="the candidate merge commit")
    parser.add_argument("--source-head", required=True, help="the pull request's own head")
    parser.add_argument("--own-branch", required=True, help="the pull request's head branch")
    args = parser.parse_args(argv)

    borrowed = preflight.borrowed_watch_coverage(
        args.root.resolve(), args.base, args.head,
        source_head=args.source_head, own_branch=args.own_branch)
    if not borrowed:
        print("vellum-borrowed-coverage: OK (no watch event in range, or one of this branch's own)")
        return 0
    parents = ", ".join(_describe(carrier) for carrier in borrowed)
    print(
        "vellum-borrowed-coverage: FAIL: every Vellum watch event covering this pull request "
        f"was added by commits of {parents}, unmerged work this branch is stacked on. "
        "When that parent merges first, the merge queue measures only this change, finds no "
        "event, and ejects the batch. This branch cannot carry its own event while stacked "
        "(duplicate family claims are refused). Re-run this check after the parent merges: "
        "merge main into this branch, write its own event with "
        "`python3 tools/scripts/vellum_watch_preflight.py --write-event`, and push.",
        file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
