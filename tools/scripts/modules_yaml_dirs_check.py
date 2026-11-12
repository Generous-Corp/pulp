#!/usr/bin/env python3
"""Every module listed in docs/status/modules.yaml has a source directory.

An entry names its directory with an optional `path:` field, relative to the
repository root (for example `integrations/pulp-native`); without one the
directory is `core/<name>`. A path must stay inside the repository and under
`core/` or `integrations/`, the two places Pulp keeps modules.

Exit 0 when every directory exists, 1 with one line per problem otherwise.
Usage: modules_yaml_dirs_check.py [--root ROOT] [--modules FILE]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path, PurePosixPath

ALLOWED_PREFIXES = ("core", "integrations")
_NAME = re.compile(r"^\s*-\s+name:\s*(\S.*?)\s*$")
_PATH = re.compile(r"^\s+path:\s*(\S.*?)\s*$")


def read_entries(text: str) -> list[tuple[str, str | None]]:
    """(name, path or None) for each `- name:` entry, in order."""
    entries: list[tuple[str, str | None]] = []
    for line in text.splitlines():
        if match := _NAME.match(line):
            entries.append((match.group(1), None))
        elif (match := _PATH.match(line)) and entries:
            name, _ = entries[-1]
            entries[-1] = (name, match.group(1))
    return entries


def problems(root: Path, entries: list[tuple[str, str | None]]) -> list[str]:
    found: list[str] = []
    for name, path in entries:
        if path is None:
            directory = PurePosixPath("core", name)
        else:
            directory = PurePosixPath(path)
            if directory.is_absolute() or ".." in directory.parts or not directory.parts:
                found.append(f"modules.yaml lists '{name}' with path '{path}', which leaves the repository")
                continue
            if directory.parts[0] not in ALLOWED_PREFIXES:
                found.append(
                    f"modules.yaml lists '{name}' with path '{path}', which is not under core/ or integrations/"
                )
                continue
        if not (root / directory).is_dir():
            found.append(f"modules.yaml lists '{name}' but {directory}/ does not exist")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--modules", type=Path)
    args = parser.parse_args(argv)
    modules = args.modules or args.root / "docs" / "status" / "modules.yaml"
    entries = read_entries(modules.read_text(encoding="utf-8"))
    if not entries:
        print(f"modules-yaml-dirs: no '- name:' entries in {modules}", file=sys.stderr)
        return 1
    found = problems(args.root, entries)
    for line in found:
        print(line)
    if not found:
        print(f"modules-yaml-dirs: OK, {len(entries)} module directories exist")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
