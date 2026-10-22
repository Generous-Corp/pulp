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

    python3 tools/scripts/fold_migration_notes.py --repo . --version 0.1007.0
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from build_migration_index import (  # noqa: E402
    FILENAME_VERSION_RE,
    UNRELEASED_DIRNAME,
    check_entry_consistency,
    load_entry,
    load_unreleased_dir,
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


def fold(repo: pathlib.Path, version: str) -> list[str]:
    """Move every unreleased note into `v<version>.md`.

    Returns the repo-relative paths it changed (the released note and each
    removed unreleased note), or an empty list when there was nothing to fold.
    Every note is read and validated before anything is written, so a bad note
    leaves the tree untouched.
    """
    if not FILENAME_VERSION_RE.match(version):
        raise ValueError(f"not a release version: {version!r}")
    notes = load_unreleased_dir(repo / UNRELEASED_DIR)
    if not notes:
        return []

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
    target.write_text(text, encoding="utf-8")

    changed = [target_rel.as_posix()]
    for note in notes:
        note.source_path.unlink()
        changed.append(note.source_path.relative_to(repo).as_posix())
    return changed


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo", type=pathlib.Path, default=pathlib.Path("."))
    ap.add_argument("--version", required=True, help="Released SDK/CLI version, X.Y.Z.")
    args = ap.parse_args(argv)
    changed = fold(args.repo.resolve(), args.version)
    for path in changed:
        print(path)
    if not changed:
        print(f"fold_migration_notes: no notes in {UNRELEASED_DIR}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
