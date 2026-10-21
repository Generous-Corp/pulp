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

An authorized bump must also pin a commit that is on pulp-planning's main
branch: a pin to a side-branch or rewritten commit silently drops every
planning commit main has that the pin lacks. The guard fetches the planning
remote's main (commit history only) and requires the new pin to be an
ancestor of it. When the remote cannot be fetched the guard fails rather than
passing unverified, except under --ci-warn-only: a CI runner that has no
credential for the private planning repository reports the unverified pin as
a loud warning and passes, so deliberate re-pins stay landable. A pin that WAS
fetched and is not on main fails in every mode.

See planning/2026-07-07-parallel-merge-land-coordination.md (T0.1).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile

# Single-file script invocation puts this directory on sys.path, so a plain
# import works from CI, hooks, and ad-hoc runs (matches the other gates).
from gate_common import git_diff_names, git_range_trailers

TRAILER = "Planning-Bump"
PLANNING_PATH = "planning"
PLANNING_BRANCH = "main"
FETCH_TIMEOUT_SECONDS = 180


class AncestryUnverified(RuntimeError):
    pass


def _git(arguments: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    return subprocess.run(["git", *arguments], text=True, capture_output=True, env=env,
                          **kwargs)


def planning_pin(head: str) -> str | None:
    """The gitlink commit `head` pins at `planning`, or None if it pins none."""
    out = _git(["ls-tree", head, "--", PLANNING_PATH])
    if out.returncode != 0:
        raise AncestryUnverified(f"cannot read the planning gitlink at {head}: "
                                 f"{out.stderr.strip()}")
    fields = out.stdout.split()
    if len(fields) >= 3 and fields[0] == "160000" and fields[1] == "commit":
        return fields[2]
    return None


def planning_remote(head: str) -> str:
    out = _git(["config", "--blob", f"{head}:.gitmodules",
                f"submodule.{PLANNING_PATH}.url"])
    url = out.stdout.strip()
    if out.returncode != 0 or not url:
        raise AncestryUnverified(f"cannot read submodule.{PLANNING_PATH}.url from "
                                 f"{head}:.gitmodules")
    return url


def pin_is_on_planning_main(pin: str, remote: str) -> bool:
    """Whether `pin` is an ancestor of `remote`'s main branch.

    Fetches main's commit graph into a throwaway repository. A pin absent from
    that history is not on main, which is the same verdict as not-an-ancestor.
    """
    with tempfile.TemporaryDirectory(prefix="planning-gitlink-") as scratch:
        if _git(["init", "--quiet", "--bare", scratch]).returncode != 0:
            raise AncestryUnverified("cannot create a scratch repository")
        ref = f"refs/remotes/origin/{PLANNING_BRANCH}"
        try:
            fetched = _git(["-C", scratch, "fetch", "--quiet", "--no-tags",
                            "--filter=tree:0", remote,
                            f"+refs/heads/{PLANNING_BRANCH}:{ref}"],
                           timeout=FETCH_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            raise AncestryUnverified(f"fetching {remote} {PLANNING_BRANCH} timed out")
        if fetched.returncode != 0:
            raise AncestryUnverified(f"cannot fetch {remote} {PLANNING_BRANCH}: "
                                     f"{fetched.stderr.strip() or 'git fetch failed'}")
        return _git(["-C", scratch, "merge-base", "--is-ancestor", pin, ref]).returncode == 0


ANCESTRY_OK = "ok"
ANCESTRY_REJECTED = "rejected"
ANCESTRY_UNVERIFIED = "unverified"


def check_pin_ancestry(head: str, remote: str | None) -> tuple[str, str]:
    """(verdict, message): ok, rejected (pin verified off main), or unverified."""
    try:
        pin = planning_pin(head)
        if pin is None:
            return ANCESTRY_OK, "planning-gitlink: head pins no planning commit"
        url = remote or planning_remote(head)
        if pin_is_on_planning_main(pin, url):
            return ANCESTRY_OK, f"planning-gitlink: pin {pin[:12]} is on {PLANNING_BRANCH} of {url}"
        return ANCESTRY_REJECTED, (
            f"planning-gitlink: the new planning pin {pin} is not an ancestor of\n"
            f"  {PLANNING_BRANCH} at {url}. Pinning it would drop every planning commit\n"
            f"  on {PLANNING_BRANCH} that it lacks. Push the planning commit to\n"
            f"  {PLANNING_BRANCH} first, then pin that {PLANNING_BRANCH} commit:\n"
            f"    git -C planning fetch origin {PLANNING_BRANCH} && "
            f"git -C planning checkout origin/{PLANNING_BRANCH}\n"
            f"    git add planning")
    except AncestryUnverified as exc:
        return ANCESTRY_UNVERIFIED, (
            f"planning-gitlink: NOT VERIFIED: {exc}.\n"
            f"  The planning pin must be checked against pulp-planning {PLANNING_BRANCH}\n"
            f"  before it can land; this guard does not pass an unverified pin. Run it\n"
            f"  where the planning remote is readable, or pass --planning-remote <url|path>.")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="origin/main",
                        help="Base ref for the merge-base diff (default: origin/main).")
    parser.add_argument("--head", default="HEAD", help="Head ref (default: HEAD).")
    parser.add_argument("--mode", choices=["report", "hint"], default="report",
                        help="report: non-zero exit on violation (default). "
                             "hint: advisory, always exits 0.")
    parser.add_argument("--planning-remote",
                        default=os.environ.get("PULP_PLANNING_REMOTE"),
                        help="Planning repository to check the pin against (default: "
                             "the head's .gitmodules URL; env PULP_PLANNING_REMOTE).")
    parser.add_argument("--ci-warn-only", action="store_true",
                        help="For CI runners that cannot read the private planning "
                             "repository: report an UNVERIFIED pin as a ::warning and "
                             "pass. A pin verified to be off main still fails.")
    args = parser.parse_args(argv)

    changed = git_diff_names(args.base, args.head)
    if "planning" not in changed:
        return 0  # gitlink untouched — nothing to guard

    trailers = git_range_trailers(args.base, args.head)
    reasons = trailers.get(TRAILER.lower(), [])
    if reasons:
        print(f"planning-gitlink: deliberate bump authorized ({reasons[0]})")
        verdict, detail = check_pin_ancestry(args.head, args.planning_remote)
        if verdict == ANCESTRY_OK:
            print(detail)
            return 0
        if verdict == ANCESTRY_UNVERIFIED and args.ci_warn_only:
            # One-line ::warning so the Actions annotation is visible on the run.
            summary = " ".join(line.strip() for line in detail.splitlines())
            print(f"::warning title=planning-gitlink NOT VERIFIED::{summary}")
            print(detail + "\n  --ci-warn-only: passing; verify the pin where the "
                  "planning remote is readable.", file=sys.stderr)
            return 0
        if args.mode == "hint":
            print("planning-gitlink (hint):\n" + detail)
            return 0
        print(detail, file=sys.stderr)
        return 1

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
