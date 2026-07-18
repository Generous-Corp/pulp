#!/usr/bin/env python3
"""Decide whether a SHA-keyed "release: stuck" tracker can be auto-closed.

`auto-release.yml`'s stranded fix/feat detector opens one tracking issue per
tip SHA when a user-facing change merges to `main` without a version bump. The
title carries only the short SHA, so — unlike the version-keyed watchdog
trackers — `watchdog-reaper.yml` cannot decide from the title alone whether the
strand has been resolved. It parses the issue body (which encodes the full tip
SHA and the uncovered surfaces) and closes the tracker only once a *later*
release tag for every uncovered surface contains the stranded commit, i.e. the
change is now reachable by consumers.

Two pure entry points, exercised by test_reap_stranded_tracker.py:

- ``parse_body(text) -> (tip_sha, surfaces)`` — extract the machine-readable
  fields the detector wrote.
- ``decide(tip_sha, surfaces, containing_tags) -> (should_close, reason)`` —
  given the tags that contain the tip commit (``git tag --contains <sha>``),
  decide close-vs-keep. Conservative: any unparseable field or any surface
  without a containing release tag leaves the tracker OPEN.

CLI (used by the workflow):

    # emit "<tip_sha>\t<surfaces_csv>" from the issue body on stdin
    python3 reap_stranded_tracker.py parse < body.md

    # read containing tag names (one per line) on stdin; print "close\t<reason>"
    # or "keep\t<reason>"
    python3 reap_stranded_tracker.py decide <tip_sha> <surfaces_csv> < tags.txt
"""

from __future__ import annotations

import re
import sys

# Surface -> regex that matches a release tag NAME for that surface.
# SDK releases tag `vX.Y.Z`; plugin releases tag `plugin-vX.Y.Z`
# (see the `create_tag` step in .github/workflows/auto-release.yml).
SURFACE_TAG_PATTERNS: dict[str, re.Pattern[str]] = {
    "sdk": re.compile(r"^v\d+\.\d+\.\d+"),
    "plugin": re.compile(r"^plugin-v\d+\.\d+\.\d+"),
}

_TIP_RE = re.compile(r"\*\*Tip commit:\*\*\s*`([0-9a-fA-F]{7,40})`")
_SURFACES_RE = re.compile(r"\*\*Uncovered surfaces:\*\*\s*`([^`]*)`")


def parse_body(text: str) -> tuple[str, list[str]]:
    """Return ``(tip_sha, surfaces)`` parsed from a stranded-tracker body.

    Missing/garbled fields come back empty so the caller keeps the issue open
    rather than guessing.
    """
    tip_match = _TIP_RE.search(text or "")
    tip_sha = tip_match.group(1).lower() if tip_match else ""

    surfaces: list[str] = []
    surf_match = _SURFACES_RE.search(text or "")
    if surf_match:
        for raw in surf_match.group(1).split(","):
            name = raw.strip().lower()
            if name:
                surfaces.append(name)
    return tip_sha, surfaces


def decide(
    tip_sha: str,
    surfaces: list[str],
    containing_tags: list[str],
) -> tuple[bool, str]:
    """Decide whether the strand is resolved.

    ``containing_tags`` is the output of ``git tag --contains <tip_sha>`` — the
    tags whose commit is a descendant of (or equal to) the stranded commit. The
    strand is resolved only when EVERY uncovered surface has at least one such
    tag, meaning a later release for that surface shipped the stranded change.

    Returns ``(should_close, reason)``. Always fail-safe: an unparseable tip
    SHA, an empty/unknown surface, or any surface still lacking a containing
    release tag returns ``(False, ...)`` so a genuinely active tracker is never
    closed.
    """
    if not tip_sha:
        return False, "could not parse tip SHA from body — leaving open"
    if not surfaces:
        return False, "could not parse uncovered surfaces from body — leaving open"

    short = tip_sha[:8]
    matched: list[str] = []
    for surface in surfaces:
        pattern = SURFACE_TAG_PATTERNS.get(surface)
        if pattern is None:
            return False, f"unrecognized surface '{surface}' — leaving open"
        hits = [t for t in containing_tags if pattern.search(t)]
        if not hits:
            return (
                False,
                f"surface '{surface}' has no released tag containing {short} yet "
                "— leaving open",
            )
        matched.extend(hits)

    tag_list = ", ".join(sorted(set(matched)))
    return (
        True,
        f"stranded commit {short} is now contained in released tag(s) "
        f"[{tag_list}] covering every uncovered surface "
        f"({', '.join(surfaces)}) — the missing bump has since shipped",
    )


def _main(argv: list[str]) -> int:
    if len(argv) < 2:
        sys.stderr.write("usage: reap_stranded_tracker.py {parse|decide} ...\n")
        return 2

    command = argv[1]
    if command == "parse":
        tip_sha, surfaces = parse_body(sys.stdin.read())
        sys.stdout.write(f"{tip_sha}\t{','.join(surfaces)}\n")
        return 0

    if command == "decide":
        if len(argv) < 4:
            sys.stderr.write(
                "usage: reap_stranded_tracker.py decide <tip_sha> <surfaces_csv> "
                "< tags.txt\n"
            )
            return 2
        tip_sha = argv[2].strip().lower()
        surfaces = [s.strip().lower() for s in argv[3].split(",") if s.strip()]
        containing_tags = [
            line.strip() for line in sys.stdin.read().splitlines() if line.strip()
        ]
        should_close, reason = decide(tip_sha, surfaces, containing_tags)
        sys.stdout.write(f"{'close' if should_close else 'keep'}\t{reason}\n")
        return 0

    sys.stderr.write(f"unknown command: {command}\n")
    return 2


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv))
