#!/usr/bin/env python3
"""Check that shared test-support sources are compiled once, not per test.

`tools/cmake/PulpTestSharedObjects.cmake` compiles consumer-independent
test-support sources (the RT allocation probe, the RT interceptor) into one
OBJECT library each and swaps every test executable's listing of the source
for those objects. This check reads the generated build.ninja and fails when:

  * a listed source is compiled by any target other than its OBJECT library
    under the build's `test/` directory (the swap missed a consumer, so it is
    back to one compile per executable); or
  * nothing links the OBJECT library's object (the swap dropped the objects,
    so consumers would link without the operator new replacement they need).

Exit codes: 0 pass, 1 fail, 77 skip (no build.ninja: not a Ninja build).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SKIP = 77
COMPILE_RE = re.compile(r"^build (\S+?): (\S*COMPILER__\S+) (\S+)")


def compile_edges(text: str) -> list[tuple[str, str]]:
    """(object, source) for every compile edge."""
    edges = []
    for line in text.splitlines():
        m = COMPILE_RE.match(line)
        if m:
            edges.append((m.group(1).replace("$:", ":"), m.group(3).replace("$:", ":")))
    return edges


def link_inputs(text: str) -> set[str]:
    """Every explicit input of every link edge."""
    inputs: set[str] = set()
    for line in text.splitlines():
        if not line.startswith("build ") or "_LINKER__" not in line:
            continue
        rest = line.split(": ", 1)[1].split(" | ")[0].split(" || ")[0]
        inputs.update(rest.split()[1:])
    return inputs


def check(text: str, shared: dict[str, str]) -> list[str]:
    """`shared` maps a source path suffix to the OBJECT library that owns it."""
    edges = compile_edges(text)
    linked = link_inputs(text)
    problems = []
    for suffix, library in shared.items():
        owners = [obj for obj, src in edges
                  if src.endswith("/" + suffix) and obj.startswith("test/")]
        shared_objs = [obj for obj in owners if f"/{library}.dir/" in obj]
        copies = [obj for obj in owners if f"/{library}.dir/" not in obj]
        if len(shared_objs) != 1:
            problems.append(f"{suffix}: expected one compile in {library}, found {len(shared_objs)}")
        if copies:
            problems.append(f"{suffix}: {len(copies)} test target(s) still compile their own "
                            f"copy, e.g. {copies[0]}")
        if shared_objs and shared_objs[0] not in linked:
            problems.append(f"{suffix}: no link step consumes {shared_objs[0]}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--shared", action="append", default=[], metavar="SOURCE=LIBRARY",
                        help="source path suffix and the OBJECT library that owns it")
    args = parser.parse_args(argv)
    ninja = args.build_dir / "build.ninja"
    if not ninja.is_file():
        print(f"SKIP: {ninja} not found (not a Ninja build)")
        return SKIP
    shared = dict(item.split("=", 1) for item in args.shared)
    if not shared:
        parser.error("at least one --shared SOURCE=LIBRARY is required")
    problems = check(ninja.read_text(encoding="utf-8", errors="replace"), shared)
    for problem in problems:
        print(f"FAIL: {problem}")
    if not problems:
        print(f"PASS: {len(shared)} shared test source(s) compiled once and linked")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
