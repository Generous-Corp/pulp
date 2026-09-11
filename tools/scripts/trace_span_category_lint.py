#!/usr/bin/env python3
"""A `gpu_*` trace span must be emitted under the `gpu` category.

`.agents/skills/trace-sql/pulp_gpu_startup_breakdown.sql` selects GPU work with
`category GLOB 'gpu*'` for every `gpu_*` span name; its only `render*` branch
demands `name GLOB 'frame*'`. A `gpu_*` span emitted under any other category
therefore matches neither branch and is invisible to every GPU query, rather
than merely mis-labelled. Nothing fails: the emitter compiles, the frame
renders, and the flushed trace looks complete.

macOS sat in exactly that state. The mac window host emitted `gpu_acquire`,
`gpu_submit` and `gpu_present` under `render`, while the Skia surfaces emitted
the same three names under `gpu`. One of the two parallel GPU paths was simply
absent from the startup breakdown, and no test could see the difference because
the offscreen capture path never runs the window host.

A source-level invariant is the only gate that covers both paths: driving the
mac window host needs a window server, so a runtime assertion self-skips on
every headless runner, and a skip is not a pass.

Escape a deliberate exception with an inline `trace-span-category-lint: skip
<reason>` comment on the same line.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# SCOPE_NAMED_ARGS must precede SCOPE_NAMED, and BEGIN_ARGS precede BEGIN, so
# the alternation matches the longer macro name first.
MACRO = re.compile(
    r"PULP_TRACE_(SCOPE_NAMED_ARGS|SCOPE_NAMED|BEGIN_ARGS|BEGIN|COUNTER)\s*\(\s*"
    r'"([^"\\]*)"\s*,\s*"([^"\\]*)"'
)

SKIP_MARKER = "trace-span-category-lint: skip"
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".hxx", ".h", ".mm", ".m"}

# The prefix rule: a span named `gpu_*` must sit in a category matching the
# SQL layer's `category GLOB 'gpu*'`.
NAME_PREFIX = "gpu_"
CATEGORY_PREFIX = "gpu"


def _is_comment(line: str) -> bool:
    """True for a line whose macro text is inside a comment, not code."""
    stripped = line.lstrip()
    return stripped.startswith(("//", "*", "/*"))


def source_files(root: Path) -> list[Path]:
    """Tracked source files under `root`.

    Uses `git ls-files` when `root` is a git worktree: a plain directory walk
    from a repo root also descends into nested checkouts (this repo carries
    full Pulp worktrees under `.claude/worktrees/`), which would lint other
    branches' code and report findings that do not exist on this one. Falls
    back to a walk for the non-git fixture trees the selftest builds.
    """
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            capture_output=True,
            check=True,
            text=True,
        ).stdout
        names = [n for n in out.split("\0") if n]
        paths = [root / n for n in names]
    except (subprocess.CalledProcessError, FileNotFoundError):
        paths = [p for p in root.rglob("*") if p.is_file()]
    return sorted(p for p in paths if p.suffix in SOURCE_SUFFIXES and p.is_file())


def violations(root: Path) -> list[tuple[Path, int, str, str, str]]:
    """(path, lineno, macro, category, name) for each mis-categorised span."""
    found: list[tuple[Path, int, str, str, str]] = []
    for path in source_files(root):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if NAME_PREFIX not in text:
            continue
        for lineno, line in enumerate(text.splitlines(), start=1):
            if SKIP_MARKER in line or _is_comment(line):
                continue
            for macro, category, name in MACRO.findall(line):
                if name.startswith(NAME_PREFIX) and not category.startswith(
                    CATEGORY_PREFIX
                ):
                    found.append(
                        (path.relative_to(root), lineno, macro, category, name)
                    )
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root to scan (default: this checkout)",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()

    found = violations(root)
    if not found:
        return 0

    print(
        f"{len(found)} GPU span(s) emitted outside the '{CATEGORY_PREFIX}' "
        f"category. Every `{NAME_PREFIX}*` span must be categorised "
        f"`{CATEGORY_PREFIX}` or the trace-SQL GPU queries cannot see it:",
        file=sys.stderr,
    )
    for path, lineno, macro, category, name in found:
        print(
            f"  {path}:{lineno}: PULP_TRACE_{macro}(\"{category}\", \"{name}\") "
            f"-> expected category \"{CATEGORY_PREFIX}\"",
            file=sys.stderr,
        )
    print(
        f"\nFix the category, or annotate the line `{SKIP_MARKER} <reason>`.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
