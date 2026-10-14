#!/usr/bin/env python3
"""Does a change touch the release path? Decides the release-path gate in merge_group.

merge_group events take no `paths:` filter, so the gate classifies the batch
itself. The path list is read from the gate's own `on.pull_request.paths`, so a
pull request and a queue batch are judged by one list that cannot drift.

    release_path_changed.py --workflow .github/workflows/release-path-pr-gate.yml \
        --files changed.txt          # prints true or false

A file list of 300 or more is treated as touching the release path: the compare
API stops listing files there, so a missing file cannot be ruled out.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

COMPARE_FILE_CAP = 300


def workflow_paths(text: str) -> list[str]:
    """The `on.pull_request.paths` globs, read without a YAML dependency.

    The runner's system Python is not guaranteed to carry PyYAML, and these
    lists are flat quoted scalars. test_release_package_step.py checks the
    result against yaml.safe_load.
    """
    lines = text.splitlines()
    out: list[str] = []
    in_pr = in_paths = False
    for line in lines:
        if re.match(r"^\S", line):
            in_pr = in_paths = False
            continue
        if re.match(r"^  pull_request:\s*$", line):
            in_pr, in_paths = True, False
            continue
        if in_pr and re.match(r"^  \S", line):
            in_pr = in_paths = False
            continue
        if in_pr and re.match(r"^    paths:\s*$", line):
            in_paths = True
            continue
        if in_paths:
            m = re.match(r"^      - ['\"]?([^'\"#]+?)['\"]?\s*(#.*)?$", line)
            if m:
                out.append(m.group(1))
            elif line.strip() and not line.strip().startswith("#"):
                in_paths = False
    return out


def glob_regex(glob: str) -> re.Pattern[str]:
    """GitHub path-filter glob: `**` crosses directories, `*` does not."""
    parts = re.split(r"(\*\*/?|\*|\?)", glob)
    rx = ""
    for part in parts:
        if part in ("**", "**/"):
            rx += ".*"
        elif part == "*":
            rx += "[^/]*"
        elif part == "?":
            rx += "[^/]"
        else:
            rx += re.escape(part)
    return re.compile(rf"^{rx}$")


def touches(files: list[str], globs: list[str]) -> bool:
    if len(files) >= COMPARE_FILE_CAP:
        return True
    patterns = [glob_regex(g) for g in globs]
    return any(p.match(f) for f in files for p in patterns)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--workflow", required=True, type=Path)
    parser.add_argument("--files", required=True, type=Path,
                        help="newline-separated changed paths")
    args = parser.parse_args(argv)
    globs = workflow_paths(args.workflow.read_text(encoding="utf-8"))
    if not globs:
        print(f"no on.pull_request.paths in {args.workflow}", file=sys.stderr)
        return 2
    files = [f for f in args.files.read_text(encoding="utf-8").splitlines() if f.strip()]
    print("true" if touches(files, globs) else "false")
    return 0


if __name__ == "__main__":
    sys.exit(main())
