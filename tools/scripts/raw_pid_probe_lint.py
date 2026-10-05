#!/usr/bin/env python3
"""raw_pid_probe_lint.py — no ``os.kill(pid, 0)`` liveness probes.

On Windows ``os.kill(pid, 0)`` sends Ctrl+C to every process on the console
rather than testing ``pid``, and a probe in any ctest-run script kills the
whole Windows ctest run. Probe through ``process_liveness.pid_alive``, which
is portable. This lint flags every ``os.kill(<anything>, 0)`` call in tracked
Python under ``tools/`` and ``test/``.

Exit codes: 0 = clean, 1 = violation, 2 = scan error.

Bypass: put ``raw-pid-probe-lint: skip <reason>`` on the call's line.
"""
from __future__ import annotations

import argparse
import ast
import pathlib
import subprocess
import sys
from typing import Callable

SKIP = "raw-pid-probe-lint: skip"


def violations(text: str) -> list[int]:
    lines = text.splitlines()
    found = []
    for node in ast.walk(ast.parse(text)):
        if not isinstance(node, ast.Call) or len(node.args) != 2:
            continue
        func = node.func
        if not (isinstance(func, ast.Attribute) and func.attr == "kill"
                and isinstance(func.value, ast.Name) and func.value.id == "os"):
            continue
        signal_arg = node.args[1]
        if isinstance(signal_arg, ast.Constant) and signal_arg.value == 0:
            if SKIP not in lines[node.lineno - 1]:
                found.append(node.lineno)
    return sorted(found)


def tracked_python(root: pathlib.Path) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z", "--", "tools/*.py", "tools/**/*.py", "test/*.py", "test/**/*.py"],
        cwd=root, capture_output=True, check=True,
    )
    return sorted({p for p in result.stdout.decode("utf-8").split("\0") if p})


def main(argv: list[str] | None = None,
         list_files: Callable[[pathlib.Path], list[str]] = tracked_python) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path("."))
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        files = list_files(root)
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"raw-pid-probe-lint: cannot list sources: {exc}", file=sys.stderr)
        return 2
    if not files:
        print(f"raw-pid-probe-lint: no Python sources found under {root}", file=sys.stderr)
        return 2
    hits = []
    for rel in files:
        path = root / rel
        if not path.is_file():
            continue
        try:
            lines = violations(path.read_text(encoding="utf-8"))
        except SyntaxError:
            continue
        hits.extend(f"{rel}:{line}" for line in lines)
    if hits:
        print("raw-pid-probe-lint: os.kill(pid, 0) sends Ctrl+C to the whole console on Windows\n")
        for hit in hits:
            print(f"  {hit}")
        print("\nUse process_liveness.pid_alive(pid) (tools/scripts/process_liveness.py).")
        return 1
    print(f"raw-pid-probe-lint: ok ({len(files)} source(s) scanned)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
