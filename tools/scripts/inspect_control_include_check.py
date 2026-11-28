#!/usr/bin/env python3
"""Check that the inspect control stack includes no UI header.

`inspect/` holds two stacks. The control stack (protocol, broker, grants,
executors, telemetry, observability) stays in audio-only Pulp, so none of its
files may include `pulp/view`, `pulp/canvas` or `pulp/render`. The view side
(overlay, window, motion tools, script inspector, Standalone UI adapters) draws
or reads the view tree and moves with the UI layer. VIEW_SIDE names it exactly:
a file outside it that includes a UI header is a violation, and a file inside it
that includes none is stale, so the list cannot drift away from the code.

The link-closure checks read the CMake graph; this one reads the sources, which
is where an include that still compiles through a transitive path shows up.

Exit codes: 0 clean, 1 a violation or a stale entry, 2 the tree could not be read.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Paths relative to inspect/. Each includes a pulp/view, pulp/canvas or
# pulp/render header today and belongs to the view side.
VIEW_SIDE = frozenset({
    "include/pulp/inspect/inspector_overlay.hpp",
    "include/pulp/inspect/inspector_window.hpp",
    "include/pulp/inspect/motion_inspector.hpp",
    "include/pulp/inspect/motion_scrubber.hpp",
    "src/control_standalone_host.cpp",
    "src/control_standalone_ui_adapter.cpp",
    "src/domain_handler.cpp",
    "src/domain_handler_inspector.cpp",
    "src/inspector_overlay.cpp",
    "src/inspector_overlay_internal.hpp",
    "src/inspector_overlay_paint.cpp",
    "src/inspector_overlay_pass_viewer.cpp",
    "src/inspector_window.cpp",
    "src/motion_inspector.cpp",
    "src/motion_scrubber.cpp",
    "src/runtime_eval_component.cpp",
    "src/source_jump.cpp",
})

UI_ROOTS = ("view", "canvas", "render")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".mm", ".m", ".inc"}
INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]pulp/([a-z_]+)/', re.MULTILINE)


def ui_includes(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return sorted({f"pulp/{root}" for root in INCLUDE_RE.findall(text) if root in UI_ROOTS})


def check(inspect_dir: Path, view_side: frozenset[str] = VIEW_SIDE) -> list[str]:
    problems: list[str] = []
    seen_view_side: set[str] = set()
    for path in sorted(inspect_dir.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        relative = path.relative_to(inspect_dir).as_posix()
        found = ui_includes(path)
        if relative in view_side:
            seen_view_side.add(relative)
            if not found:
                problems.append(f"stale: {relative} is listed as view side but includes no UI header")
        elif found:
            problems.append(f"control stack: {relative} includes {', '.join(found)}")
    for missing in sorted(view_side - seen_view_side):
        if not (inspect_dir / missing).is_file():
            problems.append(f"stale: {missing} is listed as view side but does not exist")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--inspect-dir", required=True, type=Path)
    args = parser.parse_args(argv)
    if not (args.inspect_dir / "include").is_dir():
        print(f"inspect-control-includes: INCONCLUSIVE: {args.inspect_dir} holds no include/",
              file=sys.stderr)
        return 2
    problems = check(args.inspect_dir)
    if problems:
        for problem in problems:
            print(f"inspect-control-includes: FAIL {problem}", file=sys.stderr)
        return 1
    print(f"inspect-control-includes: PASS ({len(VIEW_SIDE)} view-side files named; "
          "every other file includes no pulp/view, pulp/canvas or pulp/render header)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
