#!/usr/bin/env python3
"""Keep Pulp core from depending on an optional integration package.

Integration packages (integrations/*) sit on top of core; nothing under core/
may link, include or point at one. This is a lexical guard over core's CMake
files and sources, so it runs without a configured build. It fails closed: a
tree with no core files to scan is an error, never a pass.

Exit codes: 0 clean, 1 a boundary crossing, 2 the instrument could not run.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Targets and include roots the integration packages publish.
FORBIDDEN = (
    (re.compile(r"integrations/"), "a path under integrations/"),
    (re.compile(r"(?<![\w-])pulp::(?:native|rmlui-native|rmlui-native-metal|vellum)(?![\w-])"),
     "an integration target alias"),
    (re.compile(r"(?<![\w:-])pulp-(?:native|rmlui-native|rmlui-native-metal|vellum)(?![\w-])"),
     "an integration target"),
    (re.compile(r"[<\"]pulp/(?:native|rmlui_native|vellum)/"), "an integration header"),
    (re.compile(r"[<\"]vellum/"), "a Vellum header"),
    (re.compile(r"\bVellum(?:RmlUi)?::"), "a Vellum SDK target"),
    (re.compile(r"\bPULP_ENABLE_(?:PULP_NATIVE|RMLUI_ADAPTER|PULP_VELLUM)\b"),
     "an integration build option"),
)
SCANNED_NAMES = ("CMakeLists.txt",)
SCANNED_SUFFIXES = (".cmake", ".hpp", ".h", ".cpp", ".cc", ".mm", ".m")


def scan(root: Path) -> tuple[int, list[str]]:
    core = root / "core"
    problems: list[str] = []
    scanned = 0
    for path in sorted(core.rglob("*")):
        if not path.is_file() or not (path.name in SCANNED_NAMES or path.suffix in SCANNED_SUFFIXES):
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            raise SystemExit(f"integrations-boundary: cannot read {path}: {error}")
        scanned += 1
        for number, line in enumerate(text.splitlines(), 1):
            for pattern, what in FORBIDDEN:
                if pattern.search(line):
                    problems.append(f"{path.relative_to(root)}:{number}: core names {what}: {line.strip()}")
    return scanned, problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    args = parser.parse_args(argv)
    root = Path(args.root)
    if not (root / "core").is_dir() or not (root / "integrations").is_dir():
        print(f"integrations-boundary: {root} has no core/ and integrations/", file=sys.stderr)
        return 2
    scanned, problems = scan(root)
    if scanned == 0:
        print("integrations-boundary: scanned no core files; the check is blind", file=sys.stderr)
        return 2
    for problem in problems:
        print(f"integrations-boundary: ERROR: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"integrations_boundary_verified={scanned} core files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
