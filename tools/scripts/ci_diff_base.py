#!/usr/bin/env python3
"""Resolve the commit every diff-scoped gate in version-skill-check.yml compares against.

The anchor depends on the event that started the run:

* ``merge_group``: the queue entry's own base, ``github.event.merge_group.base_sha``.
  A merge-group commit is built on top of every entry ahead of it in the queue,
  so ``merge-base(origin/main, HEAD)`` would put those entries' commits inside
  this entry's range. A ``fix:`` commit in an entry ahead would then fail an
  innocent entry behind it, while the offending entry passes on whatever is
  ahead of *it*. ``base_sha`` is the tip this entry was stacked on, so the range
  holds exactly this entry's commits and its merge commit.
* ``pull_request``: ``merge-base(origin/<base_ref>, HEAD)``. The checked-out
  merge ref can lag the live base branch, and a gate that reads a file at the
  base tip would otherwise blame the PR for whatever the base changed since.
* anything else (``workflow_dispatch``, ``push``): ``merge-base(origin/main, HEAD)``.

A merge-group run that cannot name a base, or names one that is not an ancestor
of HEAD, fails closed: silently falling back to ``origin/main`` is exactly the
behaviour that ejected innocent entries.

Prints the resolved SHA on stdout and, with ``--github-output`` (default
``$GITHUB_OUTPUT`` when set), appends ``ref=<sha>`` to that file.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


class ResolveError(RuntimeError):
    """The base cannot be resolved; the caller must fail rather than guess."""


def _git(repo: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True,
        text=True, encoding="utf-8",
        check=False,
    )


def _rev_parse(repo: Path, rev: str) -> str:
    proc = _git(repo, "rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}")
    if proc.returncode != 0 or not proc.stdout.strip():
        raise ResolveError(f"cannot resolve {rev!r} to a commit in {repo}")
    return proc.stdout.strip()


def _merge_base(repo: Path, base: str, head: str) -> str:
    proc = _git(repo, "merge-base", base, head)
    if proc.returncode != 0 or not proc.stdout.strip():
        raise ResolveError(f"no merge-base between {base!r} and {head!r}")
    return proc.stdout.strip()


def resolve(
    repo: Path,
    event: str,
    *,
    base_ref: str = "",
    merge_group_base_sha: str = "",
    head: str = "HEAD",
) -> tuple[str, str]:
    """Return ``(sha, explanation)`` for the diff base of this run."""
    head_sha = _rev_parse(repo, head)
    if event == "merge_group":
        if not merge_group_base_sha:
            raise ResolveError(
                "merge_group run without merge_group.base_sha; refusing to fall "
                "back to origin/main, whose merge-base spans the queue entries "
                "ahead of this one"
            )
        base_sha = _rev_parse(repo, merge_group_base_sha)
        if _git(repo, "merge-base", "--is-ancestor", base_sha, head_sha).returncode != 0:
            raise ResolveError(
                f"merge_group.base_sha {base_sha} is not an ancestor of HEAD {head_sha}"
            )
        return base_sha, f"merge_group.base_sha {base_sha} (this queue entry's own base)"
    if event == "pull_request":
        if not base_ref:
            raise ResolveError("pull_request run without a base ref")
        base = f"origin/{base_ref}"
    else:
        base = "origin/main"
    sha = _merge_base(repo, base, head_sha)
    return sha, f"{sha} (merge-base of {base} and HEAD)"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], allow_abbrev=False)
    parser.add_argument("--event", required=True, help="github.event_name")
    parser.add_argument("--base-ref", default="", help="github.base_ref (pull_request)")
    parser.add_argument(
        "--merge-group-base-sha",
        default="",
        help="github.event.merge_group.base_sha (merge_group)",
    )
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--repo", default=".")
    parser.add_argument(
        "--github-output",
        default=os.environ.get("GITHUB_OUTPUT", ""),
        help="file to append ref=<sha> to (default: $GITHUB_OUTPUT)",
    )
    args = parser.parse_args(argv)

    try:
        sha, why = resolve(
            Path(args.repo),
            args.event,
            base_ref=args.base_ref,
            merge_group_base_sha=args.merge_group_base_sha,
            head=args.head,
        )
    except ResolveError as exc:
        print(f"ci_diff_base: {exc}", file=sys.stderr)
        return 2

    print(f"diff base: {why}", file=sys.stderr)
    print(sha)
    if args.github_output:
        with open(args.github_output, "a", encoding="utf-8") as fh:
            fh.write(f"ref={sha}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
