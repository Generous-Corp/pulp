#!/usr/bin/env python3
"""Refuse a hand-written `docs/migrations/vX.Y.Z.md` in a pull request.

A pull request cannot know the version its change ships in: release-please
assigns it in the release PR after the change lands. A note filed under a
guessed version goes stale whenever another release lands first, and two pull
requests guessing the same version collide on the file. Notes go in
`docs/migrations/unreleased/<topic>.md`; the SDK release PR's derived-file
refresh folds them into `vX.Y.Z.md` (`tools/scripts/release_derived_refresh.py`,
which calls `tools/scripts/fold_migration_notes.py`).

So a `vX.Y.Z.md` this range ADDS must come from that refresh commit (one
carrying the `Release-Derived-Refresh:` marker) or from a version-at-land bump
commit (`Version-Bump-Applied:`, the retired route). Editing a released note is
still allowed: a correction to a published note is not a version guess.

    python3 tools/scripts/migration_note_guard.py --base origin/main

Exit 0 when the range adds no hand-written note, 1 when it does, 2 when the
range cannot be read.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

RELEASED_NOTE = re.compile(r"^docs/migrations/v\d+\.\d+\.\d+\.md$")
APPLIED_MARKERS = ("Release-Derived-Refresh:", "Version-Bump-Applied:")


def _git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True,
                          text=True, encoding="utf-8", check=True).stdout


def hand_written_notes(root: Path, base: str, head: str = "HEAD") -> list[tuple[str, str]]:
    """(path, adding commit) for each released note the range adds by hand."""
    added = [path for path in _git(root, "diff", "--name-only", "--no-renames",
                                   "--diff-filter=A", f"{base}...{head}").splitlines()
             if RELEASED_NOTE.match(path)]
    found = []
    for path in added:
        log = _git(root, "log", "--diff-filter=A", "--format=%H%x00%B%x01",
                   f"{base}..{head}", "--", path)
        commits = [entry.strip("\n").split("\x00", 1) for entry in log.split("\x01")
                   if entry.strip()]
        for sha, message in commits:
            if not any(marker in message for marker in APPLIED_MARKERS):
                found.append((path, sha))
                break
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--root", type=Path, default=Path("."))
    args = parser.parse_args(argv)
    try:
        found = hand_written_notes(args.root, args.base, args.head)
    except subprocess.CalledProcessError as error:
        print(f"migration-note-guard: cannot read {args.base}...{args.head}: "
              f"{(error.stderr or '').strip()}", file=sys.stderr)
        return 2
    if not found:
        print("migration-note-guard: ok (no hand-written vX.Y.Z.md added)")
        return 0
    for path, sha in found:
        print(f"migration-note-guard: {path} is added by {sha[:12]}, which is not a "
              f"release-derived refresh. The release version is assigned after the change "
              f"lands, so move the note to docs/migrations/unreleased/<topic>.md, drop "
              f"`version` and `applies_if` from its frontmatter, and let the release PR "
              f"fold it (docs/migrations/unreleased/README.md).", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
