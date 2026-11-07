#!/usr/bin/env python3
"""Fail when examples/README.md and the examples/ directory disagree.

Every tracked directory directly under ``examples/`` must appear exactly once
in the first column of a table in ``examples/README.md``, and every directory
named there must exist. A row may name several directories (``a``, ``b``).
Rows in the proof-consumer table also name a planned location under
``test/consumers/``; the planned name must repeat the directory name, so the
list of pending moves cannot drift from the directories themselves.

Usage:
    python3 tools/scripts/examples_index_guard.py [--repo-root PATH]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
README = Path("examples") / "README.md"
_ROW = re.compile(r"^\|\s*((?:`[^`|]+`(?:\s*,\s*)?)+)\s*\|(.*)\|\s*$")
_NAME = re.compile(r"`([^`]+)`")
_PLANNED = re.compile(r"`test/consumers/([^`]+)`\s*\|?\s*$")


def tracked_example_dirs(root: Path) -> set[str]:
    out = subprocess.run(["git", "ls-files", "-z", "examples"], cwd=root,
                         capture_output=True, text=True, check=True).stdout
    dirs = set()
    for path in out.split("\0"):
        parts = path.split("/")
        if len(parts) > 2 and parts[0] == "examples":
            dirs.add(parts[1])
    return dirs


def indexed_rows(text: str) -> list[tuple[list[str], str | None]]:
    """Return (directories, planned test/consumers name or None) for each table row."""
    rows = []
    for line in text.splitlines():
        m = _ROW.match(line)
        if not m:
            continue
        names = _NAME.findall(m.group(1))
        planned = _PLANNED.search(line)
        rows.append((names, planned.group(1) if planned else None))
    return rows


def check(root: Path) -> list[str]:
    readme = root / README
    if not readme.is_file():
        return [f"{README} does not exist"]
    rows = indexed_rows(readme.read_text(encoding="utf-8"))
    actual = tracked_example_dirs(root)
    problems = []
    seen: dict[str, int] = {}
    for names, planned in rows:
        for name in names:
            seen[name] = seen.get(name, 0) + 1
            if name not in actual:
                problems.append(f"{README} lists '{name}' but examples/{name}/ does not exist")
        if planned is not None and (len(names) != 1 or planned != names[0]):
            problems.append(f"{README}: planned location test/consumers/{planned} must repeat "
                            f"the directory name {names}")
    for name, count in sorted(seen.items()):
        if count > 1:
            problems.append(f"{README} lists '{name}' {count} times")
    for name in sorted(actual - set(seen)):
        problems.append(f"examples/{name}/ is not listed in {README}")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    args = ap.parse_args(argv)
    problems = check(args.repo_root.resolve())
    for p in problems:
        print(f"examples-index-guard: {p}", file=sys.stderr)
    if problems:
        return 1
    print("examples-index-guard: examples/README.md lists every example directory once")
    return 0


if __name__ == "__main__":
    sys.exit(main())
