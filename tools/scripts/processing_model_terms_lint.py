#!/usr/bin/env python3
"""Lint for the reserved processing-model terminology.

Pulp keeps a hard line between its DSP *authoring* unit (`Processor`) and its
host *composition* engine (`SignalGraph`). A handful of phrases blur that line
and are always wrong; this lint flags them in docs and source so the vocabulary
in `docs/reference/processing-models.md` stays enforceable.

The forbidden set is deliberately tight (only phrases with no legitimate use) so
the check is false-positive-free and safe to gate on. It can grow as new
unambiguous anti-patterns appear.

Escape hatch: a line containing the marker `terms-lint: allow` is skipped (use
it only for prose that quotes a forbidden phrase to define it away).

Usage:
    python3 tools/scripts/processing_model_terms_lint.py [paths...]
    python3 tools/scripts/processing_model_terms_lint.py --selftest

With no paths, scans `docs/` and `core/` under the current directory. Exits 0
when clean, 1 when any forbidden phrase is found (or a self-test case fails).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ALLOW_MARKER = "terms-lint: allow"

# (compiled pattern, human message). Patterns are case-insensitive.
FORBIDDEN = [
    (
        re.compile(r"\bgraph[\s\-]plugin\b", re.IGNORECASE),
        'a plugin is a Processor, never a "graph plugin"',
    ),
    (
        re.compile(r"\bSignalGraph\s+backend\b", re.IGNORECASE),
        'the inter-node backend is GraphRuntimeExecutor, not a "SignalGraph backend"',
    ),
]

SCAN_DIRS = ("docs", "core")
SCAN_SUFFIXES = (".md", ".hpp", ".cpp", ".h", ".mm", ".cc")


def scan_text(text: str):
    """Yield (line_number, message) for each forbidden phrase in `text`."""
    for lineno, line in enumerate(text.splitlines(), start=1):
        if ALLOW_MARKER in line:
            continue
        for pattern, message in FORBIDDEN:
            if pattern.search(line):
                yield lineno, message


def iter_files(paths):
    for p in paths:
        path = Path(p)
        if path.is_file():
            yield path
        elif path.is_dir():
            for f in sorted(path.rglob("*")):
                if f.is_file() and f.suffix in SCAN_SUFFIXES:
                    yield f


def lint(paths) -> int:
    findings = 0
    for f in iter_files(paths):
        try:
            text = f.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        for lineno, message in scan_text(text):
            print(f"{f}:{lineno}: {message}")
            findings += 1
    return findings


def selftest() -> int:
    cases = [
        ("we ship a graph plugin", 1),
        ("build a graph-plugin for the rack", 1),
        ("route through the SignalGraph backend layer", 1),
        ("a graph plugin example  # terms-lint: allow", 0),
        ("write a Processor; host it in a SignalGraph", 0),
        ("the GraphRuntimeExecutor is the routing backend", 0),
        ("a SignalGraph hosts plugin nodes", 0),
    ]
    failures = 0
    for text, expected in cases:
        got = len(list(scan_text(text)))
        ok = got == expected
        if not ok:
            failures += 1
        print(f"[{'ok' if ok else 'FAIL'}] expected={expected} got={got}: {text!r}")
    if failures:
        print(f"selftest: {failures} case(s) failed")
        return 1
    print("selftest: all cases passed")
    return 0


def main(argv) -> int:
    args = argv[1:]
    if "--selftest" in args:
        return selftest()
    paths = args if args else [d for d in SCAN_DIRS if Path(d).is_dir()]
    findings = lint(paths)
    if findings:
        print(f"\n{findings} reserved-terminology violation(s) found.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
