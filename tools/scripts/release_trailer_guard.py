#!/usr/bin/env python3
"""Answer, for one commit, whether a bypass trailer withholds its release tag.

`auto-release.yml` decides post-merge whether a version bump becomes a tag, and
honours two opt-outs: `Release: skip` and a top-level
`Version-Bump: skip reason="..."`. Both are *declarations* in a commit message,
so both have to be told apart from an *example* of one — a friction report, a
guide to the trailer grammar, a PR body pasted into a commit.

That distinction is not something a line-anchored pattern can make on its own.
Indented and `>`-quoted prose moves the trailer off column zero and is excluded
for free, but a fenced code block leaves it exactly at column zero, so a fence
has to be masked before the scan. `gate_common` already draws that line for
every pre-merge gate. This is the shell-callable front door onto the same
parse, so the tagger and the gates cannot disagree about what a declaration is.

The asymmetry that makes this worth a separate entry point: a spurious tag is
loud and revocable, while a *withheld* tag is silent — the release simply does
not happen and nothing reports why. So an undecidable input exits 2 rather than
guessing, and the caller is expected to fail its step on that.

    release_trailer_guard.py --query release-skip --ref HEAD
    release_trailer_guard.py --query version-bump-skip --ref <sha>

Prints `skip` or `no-skip` on stdout. Exit 0 = verdict; 2 = undecidable.
"""

from __future__ import annotations

import argparse
import subprocess
import sys

from gate_common import (
    _parse_trailer_block,
    release_skip_declared,
    version_bump_skip_reason,
)


SKIP = "skip"
NO_SKIP = "no-skip"
UNDECIDABLE = 2


def commit_body(ref: str, repo: str | None = None) -> str | None:
    """Full message of ``ref``, or None when it cannot be read."""
    try:
        result = subprocess.run(
            ["git", "log", "-1", "--format=%B", "--end-of-options", ref],
            capture_output=True, text=True, cwd=repo,
        )
    except OSError:
        return None
    return result.stdout if result.returncode == 0 else None


def verdict(body: str, query: str) -> str:
    trailers = _parse_trailer_block(body)
    if query == "release-skip":
        return SKIP if release_skip_declared(trailers) else NO_SKIP
    return SKIP if version_bump_skip_reason(trailers) is not None else NO_SKIP


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--query", required=True, choices=("release-skip", "version-bump-skip")
    )
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--ref", default="HEAD", help="commit to inspect")
    source.add_argument(
        "--body-file", help="read the message from a file instead of git"
    )
    parser.add_argument("--repo", default=None, help="repository to read")
    args = parser.parse_args(argv)

    if args.body_file:
        try:
            with open(args.body_file, encoding="utf-8") as handle:
                body = handle.read()
        except OSError as error:
            print(f"cannot read {args.body_file}: {error}", file=sys.stderr)
            return UNDECIDABLE
    else:
        body = commit_body(args.ref, args.repo)
        if body is None:
            print(f"cannot read the message of {args.ref}", file=sys.stderr)
            return UNDECIDABLE

    print(verdict(body, args.query))
    return 0


if __name__ == "__main__":
    sys.exit(main())
