#!/usr/bin/env python3
"""
Decide whether auto-release.yml should create a tag for the given surface.

Used by .github/workflows/auto-release.yml to decide — per-surface (SDK,
plugin) — whether to create `v<version>` on the current HEAD.

## Why a separate script

The auto-release.yml history has three self-inflicted wounds in 2026-04-20:

1. **#498** — switched the diff baseline from `event.before` to
   `latest-tag`. Fixed the cascade-cancel self-heal case but broke
   `Release: skip` stickiness and revert semantics.

2. **#501** — reverted to AND-of-both (`HEAD > prev AND HEAD > tag`) to
   try to preserve all three. Re-broke self-heal: when prev == HEAD (any
   unrelated push after a prior bump landed), the first conjunct fails
   and the tag never gets created — which is what happened to v0.23.1
   after #509 + #510 merged.

3. **#510** — fixed a YAML indent bug in #501's semver helper (Python
   body at column 1 inside a 10-space-indented `run: |` block), which
   had silently prevented ANY auto-release run for the preceding 24h.

This script fixes the semantic bug that #501 introduced: decouple "was
this version bump sanctioned?" from "is the file state newer than the
tag?" so we get all three invariants right simultaneously.

## Semantic model

A version bump is *sanctioned* (deserves a tag) iff the commit that set
the version to its current value does NOT carry `Release: skip` in its
trailers. The skip trailer on the BUMP COMMIT is permanent — future
pushes that don't re-touch the version file don't re-evaluate it.

Algorithm per surface:

  1. Find bump_commit: the most recent commit on HEAD that changed the
     version in the version file to its current value.
  2. If bump_commit has `Release: skip` trailer → DO NOT TAG.
  3. Else if head_version > latest_tag_version → TAG.
  4. Else → DO NOT TAG (no-op, revert, or already-tagged).

## Test cases (all covered in test/test_auto_release_decision.py)

  * self-heal after cancel: prev=HEAD=0.23, tag=0.22, no skip on bump → TAG
  * sticky skip: prev=HEAD=0.23, tag=0.22, bump has Release: skip → NO TAG
  * revert: prev=0.23, HEAD=0.22, tag=0.23 → NO TAG
  * no-op push: prev=HEAD=tag=0.23 → NO TAG
  * first release: prev=None, HEAD=0.1.0, tag=None → TAG
  * sticky skip then unrelated push: sticky skip sticks even if 50 commits later

## CLI contract

Called from auto-release.yml with explicit inputs (resolved by bash from
git) so the decision logic itself is pure and unit-testable.

    python3 tools/scripts/auto_release_decision.py \\
        --head-version 0.23.1 \\
        --tag-version 0.23.0 \\
        --bump-commit-has-skip 0 \\
        --surface sdk

Prints a JSON object to stdout:

    {"should_tag": 1, "reason": "self-heal: head 0.23.1 > tag 0.23.0, bump commit not skipped", "surface": "sdk"}

Exit 0 on any valid decision (even NO TAG). Exit nonzero only on bad input.
"""

from __future__ import annotations

import argparse
import json
import sys


def parse_version(v: str | None) -> tuple[int, int, int] | None:
    """Parse a dot-separated semver string to a tuple. Empty/None → None."""
    if not v:
        return None
    parts = v.split(".")
    if len(parts) != 3:
        return None
    try:
        return tuple(int(p) for p in parts)  # type: ignore[return-value]
    except ValueError:
        return None


def semver_cmp(a: str | None, b: str | None) -> str:
    """Returns 'gt', 'eq', or 'lt'. Missing side treated as less than anything."""
    pa = parse_version(a)
    pb = parse_version(b)
    if pa is None and pb is None:
        return "eq"
    if pa is None:
        return "lt"
    if pb is None:
        return "gt"
    if pa > pb:
        return "gt"
    if pa < pb:
        return "lt"
    return "eq"


def decide(
    head_version: str | None,
    tag_version: str | None,
    bump_commit_has_skip: bool,
    surface: str,
) -> dict:
    """Pure decision function. Inputs come from bash/git. Testable."""
    if not head_version:
        return {
            "should_tag": 0,
            "reason": f"no {surface} version at HEAD — nothing to tag",
            "surface": surface,
        }

    if bump_commit_has_skip:
        return {
            "should_tag": 0,
            "reason": (
                f"sticky skip: bump commit for {surface} v{head_version} "
                f"carries Release: skip trailer — permanently not tagged"
            ),
            "surface": surface,
        }

    cmp = semver_cmp(head_version, tag_version)
    if cmp == "gt":
        return {
            "should_tag": 1,
            "reason": (
                f"{surface} v{head_version} > latest tag "
                f"v{tag_version or '<none>'} — tagging"
            ),
            "surface": surface,
        }

    if cmp == "eq":
        return {
            "should_tag": 0,
            "reason": (
                f"{surface} v{head_version} already tagged — no-op"
            ),
            "surface": surface,
        }

    # cmp == "lt" — downgrade (revert or accidental)
    return {
        "should_tag": 0,
        "reason": (
            f"{surface} v{head_version} < latest tag v{tag_version} — "
            f"revert or downgrade, not auto-tagging; resolve manually"
        ),
        "surface": surface,
    }


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--head-version", default="", help="Version at HEAD (e.g. 0.23.1)")
    p.add_argument("--tag-version", default="", help="Version of latest tag (e.g. 0.23.0; empty if none)")
    p.add_argument(
        "--bump-commit-has-skip",
        type=int,
        choices=[0, 1],
        default=0,
        help="1 if the commit that set HEAD's version carries `Release: skip` trailer, else 0",
    )
    p.add_argument("--surface", default="sdk", help="Label for the version surface (sdk, plugin)")
    args = p.parse_args()

    result = decide(
        head_version=args.head_version or None,
        tag_version=args.tag_version or None,
        bump_commit_has_skip=bool(args.bump_commit_has_skip),
        surface=args.surface,
    )
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
