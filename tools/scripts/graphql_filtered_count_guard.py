#!/usr/bin/env python3
"""Guard against reading `totalCount` from an `itemTypes`-filtered timeline.

GitHub's `timelineItems(itemTypes:[...])` connection filters its `nodes` and
`filteredCount`, but its `totalCount` ignores `itemTypes` and counts EVERY
timeline item on the pull request. A query such as

    timelineItems(itemTypes:[REMOVED_FROM_MERGE_QUEUE_EVENT]){totalCount}

therefore answers "how many timeline items does this PR have", never "how many
times was it dequeued": on a never-queued PR it returned 8. Read the filtered
`nodes` (and check their `__typename`) or `filteredCount` instead.

Exit codes:
    0  scanned the query surfaces and none reads totalCount from a filtered
       timeline
    1  found at least one offending selection
    2  COULD NOT SCAN: the sweep saw no filtered timelineItems selection at all,
       so an empty result would prove nothing about the guard's reach
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Tracked files under these roots that can carry a GraphQL query string.
SCAN_ROOTS = (".github", ".agents", "hooks", "tools", "test")
SCAN_SUFFIXES = {".py", ".sh", ".bash", ".yml", ".yaml", ".rs", ".js",
                 ".mjs", ".ts", ".toml"}
# This guard and its self-test spell the forbidden shape on purpose.
SELF = {"tools/scripts/graphql_filtered_count_guard.py",
        "tools/scripts/test_graphql_filtered_count_guard.py"}

_TIMELINE = re.compile(r"timelineItems\s*\(")
_TOTAL = re.compile(r"\btotalCount\b")


def _args_end(text: str, start: int) -> int | None:
    """Index just past the `)` closing the argument list opened at `start`."""
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
    return None


def _direct_selection(text: str, start: int) -> str | None:
    """The text of the selection set beginning at the first `{` after `start`,
    with nested selections removed, so only the connection's own fields remain.

    A doubled `{{` opener (a Rust or Python format string) is treated as one
    level, so a format-string query is judged by its GraphQL shape."""
    i = text.find("{", start)
    if i < 0:
        return None
    width = 1
    while i + width < len(text) and text[i + width] == "{":
        width += 1
    depth = width
    out: list[str] = []
    j = i + width
    while j < len(text) and depth > 0:
        ch = text[j]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        elif depth == width:
            out.append(ch)
        j += 1
    return "".join(out)


def find_offences(text: str) -> tuple[int, list[int]]:
    """`(filtered_selections_seen, offending_line_numbers)` for one file."""
    seen = 0
    offences: list[int] = []
    for m in _TIMELINE.finditer(text):
        end = _args_end(text, m.end() - 1)
        if end is None or "itemTypes" not in text[m.end():end]:
            continue
        seen += 1
        selection = _direct_selection(text, end)
        if selection is not None and _TOTAL.search(selection):
            offences.append(text.count("\n", 0, m.start()) + 1)
    return seen, offences


def _tracked_files(repo: Path) -> list[str]:
    res = subprocess.run(["git", "-C", str(repo), "ls-files", "--", *SCAN_ROOTS],
                         capture_output=True, text=True, check=True, encoding="utf-8")
    return [p for p in res.stdout.splitlines()
            if Path(p).suffix in SCAN_SUFFIXES and p not in SELF]


def scan(repo: Path, paths: list[str] | None = None) -> tuple[int, list[str]]:
    seen = 0
    problems: list[str] = []
    for rel in paths if paths is not None else _tracked_files(repo):
        try:
            text = (repo / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        n, lines = find_offences(text)
        seen += n
        problems += [f"{rel}:{line}" for line in lines]
    return seen, problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="*",
                    help="files to scan (default: tracked query surfaces)")
    args = ap.parse_args(argv)
    seen, problems = scan(REPO, args.paths or None)
    if problems:
        for p in problems:
            print(f"{p}: timelineItems(itemTypes:...) selects totalCount, which "
                  "counts every timeline item; read nodes or filteredCount",
                  file=sys.stderr)
        return 1
    if seen == 0:
        print("graphql-filtered-count-guard: saw no filtered timelineItems "
              "selection; the scan reached nothing", file=sys.stderr)
        return 2
    print(f"graphql-filtered-count-guard: OK - {seen} filtered timelineItems "
          "selection(s), none reads totalCount")
    return 0


if __name__ == "__main__":
    sys.exit(main())
