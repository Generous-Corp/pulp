#!/usr/bin/env python3
"""A Catch2 test name must not read as a test-spec expression.

CTest runs each discovered Catch2 case as `<binary> "<test name>"`, and Catch2
parses that argument as a test SPEC, not as a literal name.
`tools/cmake/PulpCatchAddTests.cmake` escapes `\\`, `,`, `[` and `]`, but the
spec has other operators:

  leading `~`         exclude: the entry runs every OTHER case in the binary
                      and never the one it names
  leading `*`, or     wildcard: the entry also runs every case that shares
  trailing `*`        the rest of the name
  leading `exclude:`  the long form of `~`
  leading `"`         quoted pattern
  leading `-`         read as a command-line option, not a test spec

Two `~View() ...` cases in test/test_focused_input.cpp each ran the other 740
cases of their group binary. CTest ran the two entries in parallel, so the
whole binary ran twice at once, and two property-panel cases sharing a save
path collided, failing the required macos gate.

Scans every TEST_CASE / SCENARIO / TEST_CASE_METHOD / TEMPLATE_* name in the
repository's tracked sources outside external/, joining adjacent string
literals as the compiler does. Escape a
deliberate exception with an inline `catch-test-name-guard: skip <reason>`
comment on the macro's line.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

LITERAL = r'"(?:[^"\\\n]|\\.)*"'
MACRO = re.compile(
    r"\b(?:TEST_CASE|SCENARIO|TEMPLATE_TEST_CASE|TEMPLATE_PRODUCT_TEST_CASE|"
    r"TEMPLATE_LIST_TEST_CASE|TEST_CASE_METHOD|TEMPLATE_TEST_CASE_METHOD|"
    r"TEMPLATE_LIST_TEST_CASE_METHOD|TEMPLATE_PRODUCT_TEST_CASE_METHOD)"
    r"\s*\(\s*(?:[^,\"()]+,\s*)?((?:" + LITERAL + r"\s*)+)")
SKIP_MARKER = "catch-test-name-guard: skip"
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".mm", ".h", ".hpp"}


def problem(name: str) -> str | None:
    """Why this name would be read as a spec expression, or None."""
    if name.startswith("~"):
        return "a leading '~' excludes the case: the entry runs every other case"
    if name.lower().startswith("exclude:"):
        return "a leading 'exclude:' excludes the case: the entry runs every other case"
    if name.startswith("*") or name.endswith("*"):
        return "a leading or trailing '*' is a wildcard: the entry runs other cases too"
    if name.startswith('"'):
        return "a leading '\"' starts a quoted pattern"
    if name.startswith("-"):
        return "a leading '-' is read as a command-line option"
    return None


def names(text: str):
    """Yield (line, name) for every test macro, adjacent literals joined."""
    for match in MACRO.finditer(text):
        parts = re.findall(LITERAL, match.group(1))
        yield text.count("\n", 0, match.start()) + 1, "".join(p[1:-1] for p in parts)


def sources(root: Path) -> list[Path]:
    """Tracked C/C++/Objective-C++ sources, outside vendored code."""
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True,
                             check=True).stdout.decode("utf-8", "replace")
        relative = [name for name in out.split("\0") if name]
    except (OSError, subprocess.CalledProcessError):
        relative = [str(p.relative_to(root)) for p in root.rglob("*")]
    return [root / name for name in sorted(relative)
            if Path(name).suffix in SOURCE_SUFFIXES and not name.startswith("external/")]


def scan(root: Path):
    """Return (violations, names_seen) over the tracked sources."""
    found = []
    seen = 0
    for path in sources(root):
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        lines = text.splitlines()
        for line, name in names(text):
            seen += 1
            why = problem(name)
            if why and SKIP_MARKER not in lines[line - 1]:
                found.append((path.relative_to(root), line, name, why))
    return found, seen


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2],
                        help="repository root to scan (default: this checkout)")
    args = parser.parse_args(argv)
    found, seen = scan(args.root.resolve())
    # A zero finding is only meaningful against a non-zero control: no names
    # seen means the scan missed the tests, not that they are clean.
    if not seen:
        print("catch-test-name-guard: no test names found; the scan "
              "is not looking at the tests", file=sys.stderr)
        return 2
    for path, line, name, why in found:
        print(f"{path}:{line}: \"{name}\": {why}")
    if found:
        print(f"\ncatch-test-name-guard: {len(found)} test name(s) Catch2 reads as a "
              "test-spec expression; rename them (e.g. '~View()' -> 'View destructor').",
              file=sys.stderr)
        return 1
    print(f"catch-test-name-guard: OK ({seen} test names)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
