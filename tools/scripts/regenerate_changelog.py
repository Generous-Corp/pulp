#!/usr/bin/env python3
"""Regenerate CHANGELOG.md from git tags.

Walks every `v*` tag in reverse-chronological order, extracts merged PRs
in each tag's range, and emits a Keep-a-Changelog-flavoured document
with release-page backlinks.

Versions that contain zero user-facing merges (only `chore: bump` commits)
are omitted — we don't publish an empty entry.

Idempotent: running twice produces byte-identical output.

Spec: issue #262.
"""

from __future__ import annotations

import argparse
import datetime as dt
import re
import subprocess
import sys
from pathlib import Path
from typing import NamedTuple


REPO_URL = "https://github.com/danielraffel/pulp"

# Commits we treat as "no user-visible change" and strip from changelog
# rendering. These are the side-effects of the release machinery, not
# work the user wants to see.
_SKIP_PATTERNS = (
    re.compile(r"^chore: bump .*version", re.IGNORECASE),
    re.compile(r"^chore\(release\): ", re.IGNORECASE),
    re.compile(r"^bump .*to v?\d+\.\d+\.\d+$", re.IGNORECASE),
)


class Entry(NamedTuple):
    version: str     # "0.13.1"
    tag: str         # "v0.13.1"
    date: str        # "2026-04-16"
    prs: list[tuple[int, str]]  # [(259, "fix(cli): ..."), ...]


def run(cmd: list[str]) -> str:
    return subprocess.check_output(cmd, text=True).strip()


def discover_tags() -> list[str]:
    """Return `v*` tags in reverse semver order (newest first)."""
    out = run(["git", "tag", "--list", "v*", "--sort=-v:refname"])
    return [t for t in out.splitlines() if re.fullmatch(r"v\d+\.\d+\.\d+", t)]


def tag_date(tag: str) -> str:
    iso = run(["git", "log", "-1", "--format=%cI", tag])
    return iso[:10]  # YYYY-MM-DD


def merges_between(previous: str | None, current: str) -> list[tuple[int, str]]:
    """List (PR number, subject) for PR landings in (previous, current].

    Pulp squashes PRs into single commits on main, so we look at every
    commit (not just `--merges`) and pick the ones whose subject ends
    with `(#N)` — GitHub's convention for squash merges. We also fall
    back to old `Merge pull request #N from owner/branch` for history
    that predates the squash policy.
    """
    range_ = f"{previous}..{current}" if previous else current
    out = run([
        "git", "log", range_, "--first-parent", "--pretty=format:%s",
    ])
    prs: list[tuple[int, str]] = []
    seen: set[int] = set()
    for line in out.splitlines():
        # Squash-merge: "docs: pulp version ... (#257)"
        m = re.search(r"\s*\(#(\d+)\)\s*$", line)
        if m:
            number = int(m.group(1))
            subject = line[: m.start()].rstrip()
        else:
            # Legacy merge-commit: "Merge pull request #N from owner/branch"
            m = re.match(r"^Merge pull request #(\d+) from .+?/(.+)$", line)
            if not m:
                continue
            number = int(m.group(1))
            subject = m.group(2).replace("-", " ").strip() or "Merge"
        if number in seen:
            continue
        if any(p.search(subject) for p in _SKIP_PATTERNS):
            continue
        seen.add(number)
        prs.append((number, subject))
    return prs


def build_entries() -> list[Entry]:
    tags = discover_tags()
    entries: list[Entry] = []
    for i, tag in enumerate(tags):
        prev = tags[i + 1] if i + 1 < len(tags) else None
        prs = merges_between(prev, tag)
        if not prs:
            # Omit versions with no user-visible change.
            continue
        entries.append(Entry(
            version=tag[1:],
            tag=tag,
            date=tag_date(tag),
            prs=prs,
        ))
    return entries


def _anchor(entry: Entry) -> str:
    """Stable HTML anchor id for a version heading.

    GitHub auto-slugs `## [0.13.1] - 2026-04-16` unpredictably once you
    add brackets or em-dashes; emit an explicit `<a id=…>` tag so links
    into the file are deterministic across renderers.
    """
    return f"v{entry.version.replace('.', '')}"


def render_changelog(entries: list[Entry]) -> str:
    lines: list[str] = [
        "# Changelog",
        "",
        "All notable changes to Pulp are documented here. Each entry links",
        f"to its [GitHub Release]({REPO_URL}/releases).",
        "",
    ]
    for e in entries:
        # Explicit anchor so inbound links from GitHub Releases work
        # whatever GitHub's slugifier does with the bracket + date.
        lines.append(f'<a id="{_anchor(e)}"></a>')
        # Hyphen (not em-dash) — renders the same but keeps the slug
        # simple for any consumer that still relies on the auto anchor.
        lines.append(f"## [{e.version}] - {e.date}")
        lines.append("")
        for number, subject in e.prs:
            lines.append(f"- {subject} ([#{number}]({REPO_URL}/pull/{number}))")
        lines.append("")
    # Reference-link targets at the bottom — one per rendered version.
    for e in entries:
        lines.append(f"[{e.version}]: {REPO_URL}/releases/tag/{e.tag}")
    lines.append("")
    return "\n".join(lines)


def render_release_notes(entry: Entry, prev: Entry | None) -> str:
    lines: list[str] = [
        f"## What's new in {entry.tag}",
        "",
    ]
    for number, subject in entry.prs:
        lines.append(f"- {subject} (#{number})")
    lines.append("")
    lines.append(
        f"**Full changelog:** [CHANGELOG.md § {entry.version}]"
        f"({REPO_URL}/blob/main/CHANGELOG.md#{_anchor(entry)})"
    )
    if prev:
        lines.append(f"**Previous release:** [{prev.tag}]({REPO_URL}/releases/tag/{prev.tag})")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--changelog", default="CHANGELOG.md",
        help="path to the changelog file to overwrite",
    )
    parser.add_argument(
        "--release-notes", metavar="TAG",
        help="print release notes for TAG to stdout and exit",
    )
    parser.add_argument(
        "--check", action="store_true",
        help="exit 0 if CHANGELOG.md matches the generated output, 1 otherwise",
    )
    args = parser.parse_args()

    entries = build_entries()

    if args.release_notes:
        by_tag = {e.tag: e for e in entries}
        target = by_tag.get(args.release_notes)
        if not target:
            print(f"no tag {args.release_notes!r} with user-visible merges", file=sys.stderr)
            return 2
        idx = entries.index(target)
        prev = entries[idx + 1] if idx + 1 < len(entries) else None
        sys.stdout.write(render_release_notes(target, prev))
        return 0

    output = render_changelog(entries)
    path = Path(args.changelog)

    if args.check:
        current = path.read_text() if path.exists() else ""
        if current == output:
            return 0
        print(f"{path} is out of date. Run `{sys.argv[0]}` to regenerate.",
              file=sys.stderr)
        return 1

    path.write_text(output)
    print(f"Wrote {path} ({len(entries)} versions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
