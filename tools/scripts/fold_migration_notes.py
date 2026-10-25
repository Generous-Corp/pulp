#!/usr/bin/env python3
"""Fold `docs/migrations/unreleased/*.md` into the note for a released version.

A pull request cannot know the version its change will ship in: version-at-land
assigns it after the change lands, in merge order. So authors write an
unreleased note (no `version`, no `applies_if`), and the bump commit that
assigns the SDK/CLI version calls `fold()` to move every unreleased note into
`docs/migrations/v<version>.md`, writing the version and the canonical
`applies_if` that `build_migration_index.py` requires.

When `v<version>.md` already exists (a note filed under that version by hand),
the unreleased notes are appended to it: `breaking` becomes true if any part is
breaking, the summaries are joined, and the bodies follow the existing body in
file-name order.

A note whose change is already in an earlier release joins that release's note
instead (see `shipped_version`). That happens on the PR route: the bump PR is
cut at one commit and merges later, so a change merged in between ships in the
release tagged on that merge while its note is still unreleased.

    python3 tools/scripts/fold_migration_notes.py --repo . --version 0.1007.0
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile
from typing import Callable

HERE = pathlib.Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from build_migration_index import (  # noqa: E402
    FILENAME_VERSION_RE,
    UNRELEASED_DIRNAME,
    check_entry_consistency,
    load_entry,
    load_unreleased_dir,
    semver_key,
)

MIGRATIONS_DIR = pathlib.PurePosixPath("docs/migrations")
UNRELEASED_DIR = MIGRATIONS_DIR / UNRELEASED_DIRNAME


def _toml_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(version: str, breaking: bool, summary: str, body: str) -> str:
    """A released note in the shape `build_migration_index.py` accepts."""
    applies_if = f"cli_version_from < {version} && cli_version_to >= {version}"
    lines = [
        "---",
        f"version = {_toml_string(version)}",
        f"breaking = {'true' if breaking else 'false'}",
        f"applies_if = {_toml_string(applies_if)}",
    ]
    if summary:
        lines.append(f"summary = {_toml_string(summary)}")
    lines += ["---", "", body.strip("\n"), ""]
    return "\n".join(lines)


def shipped_version(repo: pathlib.Path, rel: str) -> str | None:
    """The earliest released SDK version whose tag contains the commit that
    added ``rel``, or None when no release tag contains it yet.

    On the PR route a bump PR is cut at one commit and merges later, so a
    change merged in between ships in the release tagged on that merge while
    its note is still unreleased; the next fold must key it to that release.
    """
    import subprocess

    def git(*args: str) -> str:
        return subprocess.run(["git", "-C", str(repo), *args], capture_output=True,
                              text=True, encoding="utf-8", check=True).stdout

    # A shallow clone can miss the commit that added the note or the tags that
    # contain it, and would then key a shipped note to the wrong release. Fall
    # back to the bump's own version, the behaviour before tags were consulted.
    if git("rev-parse", "--is-shallow-repository").strip() == "true":
        return None
    added = git("log", "--diff-filter=A", "--format=%H", "-1", "--", rel).strip()
    if not added:
        return None
    tags = git("tag", "--contains", added, "--list", "v[0-9]*").split()
    versions = [t[1:] for t in tags if FILENAME_VERSION_RE.match(t[1:])]
    return min(versions, key=semver_key) if versions else None


def _fold_into(repo: pathlib.Path, version: str, notes: list) -> list[str]:
    target_rel = MIGRATIONS_DIR / f"v{version}.md"
    target = repo / target_rel
    breaking = any(n.breaking for n in notes)
    summaries = [n.summary for n in notes]
    bodies = [n.body for n in notes]
    if target.exists():
        existing = load_entry(target)
        breaking = breaking or existing.breaking
        summaries = [existing.summary] + summaries if existing.summary else summaries
        bodies = [existing.body] + bodies
    text = render(version, breaking, "; ".join(summaries),
                  "\n\n".join(b.strip("\n") for b in bodies))
    # Prove the result is a note the CLI index accepts before touching the
    # tree, so a failure folds nothing.
    with tempfile.TemporaryDirectory() as scratch:
        probe = pathlib.Path(scratch) / target.name
        probe.write_text(text, encoding="utf-8")
        check_entry_consistency(load_entry(probe))
    return [target_rel.as_posix(), text]


def fold(repo: pathlib.Path, version: str,
         shipped_in: Callable[[str], str | None] | None = None) -> list[str]:
    """Move every unreleased note into the note for the release it ships in.

    That is `v<version>.md`, unless ``shipped_in`` reports that a note's change
    is already in an earlier release (see :func:`shipped_version`), in which
    case it joins that release's note. Returns the repo-relative paths it
    changed (each released note written and each removed unreleased note), or
    an empty list when there was nothing to fold. Every note is read and every
    result validated before anything is written, so a bad note leaves the tree
    untouched.
    """
    if not FILENAME_VERSION_RE.match(version):
        raise ValueError(f"not a release version: {version!r}")
    notes = load_unreleased_dir(repo / UNRELEASED_DIR)
    if not notes:
        return []

    groups: dict[str, list] = {}
    for note in notes:
        target = version
        if shipped_in is not None:
            shipped = shipped_in(note.source_path.relative_to(repo).as_posix())
            if shipped and semver_key(shipped) < semver_key(version):
                target = shipped
        groups.setdefault(target, []).append(note)

    writes = [_fold_into(repo, target, group)
              for target, group in sorted(groups.items(), key=lambda g: semver_key(g[0]))]
    changed = []
    for rel, text in writes:
        (repo / rel).write_text(text, encoding="utf-8")
        changed.append(rel)
    for note in notes:
        note.source_path.unlink()
        changed.append(note.source_path.relative_to(repo).as_posix())
    return changed


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", type=pathlib.Path, default=pathlib.Path("."))
    ap.add_argument("--version", required=True, help="Released SDK/CLI version, X.Y.Z.")
    args = ap.parse_args(argv)
    repo = args.repo.resolve()
    changed = fold(repo, args.version, lambda rel: shipped_version(repo, rel))
    for path in changed:
        print(path)
    if not changed:
        print(f"fold_migration_notes: no notes in {UNRELEASED_DIR}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
