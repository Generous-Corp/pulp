#!/usr/bin/env python3
"""planning_gitlink_guard.py — reject an unintended `planning` submodule bump.

A feature PR that is not deliberately re-pinning the planning snapshot should
not move the `planning` gitlink. Accidental bumps are a real, expensive class:
a `git reset --hard origin/main` leaves the submodule *working tree* at the old
commit (reset moves the index gitlink, not the submodule checkout), and a later
`git add -A` re-stages that drifted pointer. This silently rides along in the
commit and manufactures phantom "conflicts" against a moving main — it burned
hours landing a PR on 2026-07-06/07.

`.gitmodules` `ignore = all` does NOT help: it only hides the pointer from
`git status`/`git diff`; `git add -A` still stages it (verified), which would
make the accident silent instead of preventing it. A gate is the right defense.

This gate fails when the diff range moves the `planning` gitlink and no commit
in the range carries a `Planning-Bump:` trailer authorizing it. Deliberate
re-pins stay easy — add one trailer:

    Planning-Bump: reason="pin newer planning snapshot for <x>"

Recovery for an accidental bump:

    git restore --staged --worktree planning && git submodule update planning

See planning/2026-07-07-parallel-merge-land-coordination.md (T0.1).
"""

from __future__ import annotations

import argparse
import sys

# Single-file script invocation puts this directory on sys.path, so a plain
# import works from CI, hooks, and ad-hoc runs (matches the other gates).
from gate_common import git_diff_names, git_range_trailers

TRAILER = "Planning-Bump"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="origin/main",
                        help="Base ref for the merge-base diff (default: origin/main).")
    parser.add_argument("--head", default="HEAD", help="Head ref (default: HEAD).")
    parser.add_argument("--mode", choices=["report", "hint"], default="report",
                        help="report: non-zero exit on violation (default). "
                             "hint: advisory, always exits 0.")
    args = parser.parse_args(argv)

    changed = git_diff_names(args.base, args.head)
    if "planning" not in changed:
        return 0  # gitlink untouched — nothing to guard

    trailers = git_range_trailers(args.base, args.head)
    reasons = trailers.get(TRAILER.lower(), [])
    if reasons:
        print(f"planning-gitlink: deliberate bump authorized ({reasons[0]})")
        return 0

    message = (
        "planning-gitlink: this PR moves the `planning` submodule pointer but no\n"
        f"  `{TRAILER}:` trailer authorizes it.\n"
        "  - If it is ACCIDENTAL (a `git reset --hard` + `git add -A` re-staged the\n"
        "    drifted submodule), drop it:\n"
        "      git restore --staged --worktree planning && git submodule update planning\n"
        "  - If it is DELIBERATE (re-pinning the planning snapshot), add a trailer to\n"
        "    any commit in the range:\n"
        f"      {TRAILER}: reason=\"pin newer planning snapshot for <x>\""
    )
    if args.mode == "hint":
        print("planning-gitlink (hint):\n" + message)
        return 0
    print(message, file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
