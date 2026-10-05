#!/usr/bin/env python3
"""A test's temp path must not be keyed on a clock reading alone.

CTest runs each Catch2 case as its own process and runs many at once. A temp
directory or file named `<prefix>-<steady_clock ticks>` is shared whenever two
cases read the same tick (a duplicate turns up within a few thousand
concurrent `steady_clock` reads on Apple silicon), and the cases then read and
delete each other's files. That ejected merge-queue batches twice: the
browser-capture fixture accepted a capture with drifted counts, and two
property-panel runs saved over each other.

Use `pulp::test::make_unique_temp_dir` (test/support/unique_temp_dir.hpp): the
name carries the process id and a per-process serial, and the directory is
used only when that call created it. For a single file, `unique_temp_path`
gives the same pid + serial name without creating anything.

A site is a `time_since_epoch` read under test/ with `temp_directory_path`
within five lines of it and no process id, serial (`fetch_add`), random source
or the shared helper in the same window. Escape a deliberate exception with
`clock-only-temp-key-guard: skip <reason>` on the clock line. Sites that
remain on purpose are counted per file in clock_only_temp_key_guard.json; those
counts may only shrink.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

WINDOW = 5
UNIQUE = re.compile(
    r"getpid|_getpid|process_id|\bpid\b|fetch_add|random_device|mt19937|mkdtemp|"
    r"make_unique_temp_dir|unique_temp_dir|unique_temp_path|uuid", re.IGNORECASE)
SKIP_MARKER = "clock-only-temp-key-guard: skip"
SUFFIXES = {".cpp", ".cc", ".cxx", ".mm", ".h", ".hpp"}
LEDGER = Path(__file__).with_name("clock_only_temp_key_guard.json")


def sources(root: Path) -> list[Path]:
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "test"],
                             capture_output=True, check=True).stdout.decode("utf-8", "replace")
        names = [name for name in out.split("\0") if name]
    except (OSError, subprocess.CalledProcessError):
        names = [str(p.relative_to(root)) for p in (root / "test").rglob("*")]
    return [root / name for name in sorted(names) if Path(name).suffix in SUFFIXES]


def sites(lines: list[str]) -> list[int]:
    """1-based lines of clock reads that key a temp path with nothing else."""
    found = []
    for index, line in enumerate(lines):
        if "time_since_epoch" not in line or SKIP_MARKER in line:
            continue
        window = "\n".join(lines[max(0, index - WINDOW):index + WINDOW + 1])
        if "temp_directory_path" in window and not UNIQUE.search(window):
            found.append(index + 1)
    return found


def scan(root: Path):
    """Return (violations, temp_paths_seen)."""
    found = []
    seen = 0
    for path in sources(root):
        if not path.is_file():
            continue
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        seen += sum("temp_directory_path" in line for line in lines)
        found += [(path.relative_to(root), line) for line in sites(lines)]
    return found, seen


def load_ledger(path: Path) -> dict[str, int]:
    if not path.is_file():
        return {}
    data = json.loads(path.read_text())
    return {name: int(entry["count"]) for name, entry in data.get("sites", {}).items()}


def judge(found, ledger: dict[str, int]) -> list[str]:
    """Problems: sites beyond a file's listed count, and counts that went stale."""
    counts: dict[str, list[int]] = {}
    for path, line in found:
        counts.setdefault(str(path), []).append(line)
    problems = []
    for name, lines in sorted(counts.items()):
        allowed = ledger.get(name, 0)
        if len(lines) > allowed:
            problems += [f"{name}:{line}: temp path keyed on a clock reading alone; use "
                         "pulp::test::make_unique_temp_dir (test/support/unique_temp_dir.hpp)"
                         for line in lines]
    for name, allowed in sorted(ledger.items()):
        actual = len(counts.get(name, []))
        if actual < allowed:
            problems.append(f"{name}: listed with {allowed} clock-only site(s) but has {actual}; "
                            f"lower its count in {LEDGER.name} (delete the entry at zero)")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--ledger", type=Path, default=LEDGER)
    args = parser.parse_args(argv)
    found, seen = scan(args.root.resolve())
    # No temp paths seen means the scan is not looking at the tests.
    if not seen:
        print("clock-only-temp-key-guard: no temp_directory_path use found under test/; "
              "the scan is not looking at the tests", file=sys.stderr)
        return 2
    problems = judge(found, load_ledger(args.ledger))
    for problem in problems:
        print(problem)
    if problems:
        print(f"\nclock-only-temp-key-guard: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    print(f"clock-only-temp-key-guard: OK ({seen} temp_directory_path uses, "
          f"{len(found)} listed site(s) remain)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
