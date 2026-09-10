#!/usr/bin/env python3
"""Enforce the canvas path-batching flush rule in web-compat-canvas.js.

`moveTo`/`lineTo` do not emit a bridge command immediately; they accumulate a
pending run that `_fp()` ships as one `canvasPathPolyline` call. That is only
safe while the deferral never becomes a reordering, which requires every other
method that emits a bridge command to call `_fp()` first.

The failure mode this guards is quiet: a method that emits without flushing
interleaves a path fragment into the wrong paint state, so the frame renders
with a stray or mis-styled subpath. Nothing throws, and a whole-image pixel
score is a poor detector for it. Checking the rule mechanically is therefore the
gate; code review is not.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

# The batching machinery itself. `moveTo`/`lineTo` open and extend the pending
# run, `rect` seeds one directly, and `_fp` is the flush.
EXEMPT = {"moveTo", "lineTo", "rect", "_fp"}

METHOD = re.compile(
    r"CanvasRenderingContext2D\.prototype\.(\w+)\s*=\s*function\s*\([^)]*\)\s*\{\s*$"
)
# A bridge global being *called*. `typeof canvasFoo === "function"` is a guard,
# not an emission, and deliberately does not match.
EMIT = re.compile(r"\bcanvas[A-Z]\w*\s*\(")


def method_bodies(lines: list[str]):
    """Yield (name, body_lines) for each prototype method, by brace depth."""
    for i, line in enumerate(lines):
        m = METHOD.match(line)
        if not m:
            continue
        depth, j, body = 1, i + 1, []
        while j < len(lines) and depth > 0:
            depth += lines[j].count("{") - lines[j].count("}")
            if depth <= 0:
                break
            body.append(lines[j])
            j += 1
        yield m.group(1), body


def check(path: pathlib.Path) -> list[str]:
    lines = path.read_text().split("\n")
    emitters, missing = [], []
    for name, body in method_bodies(lines):
        if not any(EMIT.search(b) for b in body):
            continue
        emitters.append(name)
        if name in EXEMPT:
            continue
        if not any("this._fp()" in b for b in body):
            missing.append(name)

    problems = [
        f"{name}() emits a bridge command without calling this._fp() first"
        for name in missing
    ]
    # A rule that matched nothing would pass silently and prove nothing, so the
    # absence of violations is only meaningful alongside a non-zero population.
    if not emitters:
        problems.append(
            "found no bridge-emitting methods at all — the parser no longer "
            "matches this file, so a clean result here would be meaningless"
        )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--file",
        default="core/view/js/web-compat-canvas.js",
        type=pathlib.Path,
    )
    args = ap.parse_args()
    if not args.file.exists():
        print(f"canvas-path-flush: {args.file} not found", file=sys.stderr)
        return 1
    problems = check(args.file)
    if problems:
        print("canvas-path-flush: FAILED", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(
            "\n  moveTo/lineTo defer their bridge call into a pending run. Any\n"
            "  method that emits a bridge command must call this._fp() first, or\n"
            "  the run lands after it and the path is drawn in the wrong state.",
            file=sys.stderr,
        )
        return 1
    print("canvas-path-flush: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
