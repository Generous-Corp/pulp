#!/usr/bin/env python3
"""Reject a newly added real-clock read in a Python test under tools/.

A test that builds an expectation from the real clock while the code under test
runs against a pinned date stops agreeing with itself the day the real date
crosses a window boundary. Nothing in the test changed, so it passes for weeks
and then fails every run at once. That is how a fixture date derived from
``datetime.now()`` next to a checker pinned with ``--today`` turned the required
versioning check red for every merge group at 00:00 UTC.

This lint scans the ADDED or EDITED lines of ``tools/**/test_*.py`` and
``tools/**/*_test.py`` and flags any that read the real clock:
``datetime.now``, ``datetime.today``, ``datetime.utcnow``, ``date.today``,
``time.time``, ``time.time_ns`` and ``time.localtime``.

A line is accepted when it carries an inline marker naming why the read is safe::

    started = time.time()  # clock-ok: elapsed-time measurement, never compared to a date

or when it matches an entry in the baseline ``clock_in_tests_baseline.json``
(frozen existing uses, matched by file and stripped line text, each with a
reason) or its file is listed under ``files`` with a reason. Only the diff is
scanned, so the existing population never blocks a push; the baseline exists so
an unrelated edit to a baselined line does not either.

Detection limits: the scan is textual. It does not follow an alias
(``from time import time as now``) or a clock read hidden in a helper module,
and a docstring that mentions ``datetime.now(`` is flagged because only
comment-only lines are skipped. The marker is the escape for the latter.

Usage::

    clock_in_tests_lint.py [--base origin/main] [--head HEAD]
    clock_in_tests_lint.py --all            # scan every tracked test file
    clock_in_tests_lint.py --emit-baseline  # print a baseline for the whole tree

Exit status: 0 clean, 1 a flagged line.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = ROOT / "tools/scripts/clock_in_tests_baseline.json"

_CLOCK = re.compile(
    r"\b(?:datetime\.(?:now|today|utcnow)|date\.today|time\.(?:time|time_ns|localtime))\s*\("
)
_MARKER = re.compile(r"clock-ok:\s*\S")


def in_scope(rel: str) -> bool:
    return rel.startswith("tools/") and rel.endswith(".py") and (
        Path(rel).name.startswith("test_") or rel.endswith("_test.py")
    )


def flagged(line: str) -> bool:
    """True when the line reads the real clock without an inline marker."""
    if line.lstrip().startswith("#"):
        return False
    if _MARKER.search(line):
        return False
    return _CLOCK.search(line) is not None


def load_baseline(path: Path = BASELINE) -> tuple[dict[str, str], set[tuple[str, str]]]:
    if not path.exists():
        return {}, set()
    data = json.loads(path.read_text(encoding="utf-8"))
    files = {e["path"]: e["reason"] for e in data.get("files", [])}
    lines = {(e["path"], e["line"]) for e in data.get("lines", [])}
    return files, lines


def scan_lines(rel: str, lines: list[tuple[int, str]], baseline=None) -> list[str]:
    files, entries = baseline if baseline is not None else load_baseline()
    if rel in files:
        return []
    out = []
    for number, text in lines:
        if flagged(text) and (rel, text.strip()) not in entries:
            out.append(f"{rel}:{number}: reads the real clock: {text.strip()}")
    return out


def _git(args: list[str]) -> str:
    return subprocess.run(
        ["git", "-C", str(ROOT), *args], check=True, capture_output=True, text=True
    ).stdout


def added_lines(base: str, head: str | None) -> dict[str, list[tuple[int, str]]]:
    merge_base = _git(["merge-base", base, head or "HEAD"]).strip()
    cmd = ["diff", "-U0", "--no-color", "--diff-filter=AM", merge_base]
    if head:
        cmd.append(head)
    result: dict[str, list[tuple[int, str]]] = {}
    current = None
    number = 0
    for raw in _git(cmd).splitlines():
        if raw.startswith("+++ b/"):
            current = raw[6:]
        elif raw.startswith("+++ "):
            current = None
        elif raw.startswith("@@"):
            number = int(re.search(r"\+(\d+)", raw).group(1))
        elif raw.startswith("+") and current:
            if in_scope(current):
                result.setdefault(current, []).append((number, raw[1:]))
            number += 1
    return result


def tracked_lines() -> dict[str, list[tuple[int, str]]]:
    result: dict[str, list[tuple[int, str]]] = {}
    for rel in _git(["ls-files", "tools"]).splitlines():
        if in_scope(rel) and (ROOT / rel).is_file():
            text = (ROOT / rel).read_text(encoding="utf-8", errors="replace")
            result[rel] = list(enumerate(text.splitlines(), 1))
    return result


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--head", default=None, help="scan committed range only")
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--emit-baseline", action="store_true")
    args = parser.parse_args(argv)

    if args.emit_baseline:
        entries = []
        for rel, lines in sorted(tracked_lines().items()):
            for _, text in lines:
                if flagged(text):
                    entries.append({"path": rel, "line": text.strip(),
                                    "reason": "existing use when the lint landed"})
        print(json.dumps({"files": [], "lines": entries}, indent=2))
        return 0

    found: list[str] = []
    source = tracked_lines() if args.all else added_lines(args.base, args.head)
    baseline = load_baseline()
    for rel, lines in sorted(source.items()):
        found.extend(scan_lines(rel, lines, baseline))
    if found:
        print("\n".join(found), file=sys.stderr)
        print(
            "clock_in_tests_lint: a test that reads the real clock stops passing the day the date\n"
            "crosses a window it assumed. Pin the date, or add `# clock-ok: <reason>` to the line.",
            file=sys.stderr,
        )
        return 1
    print("clock_in_tests_lint: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
