#!/usr/bin/env python3
"""Assert the diff-scoped clang-format gate is wired the same way on every
surface, and that none of them can report a missing binary as a formatting
verdict.

format_changed.sh distinguishes exit 1 (a touched line is not clean) from
exit 3 (no clang-format 21 available). The value of that distinction is only
realised if each caller keeps it: a hook that maps 3 to `fail=1`, or a
workflow that prints the formatting error for 3, has turned an infrastructure
gap into "your code is misformatted" — the false-verdict class this repo keeps
paying for. Three callers, three text assertions each, so the wiring cannot
drift silently.

The hook and gates.sh block on exit 1 by default (an unformatted touched line
otherwise reaches CI and costs a push and a re-review round), with
PULP_ENFORCE_PREPUSH_FORMAT=0 as the one-push demotion. That is checked by
running each caller's own `if [ -f "$FMT" ] ... fi` block in bash against a
stub format_changed.sh, not by reading it.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PREPUSH = ROOT / ".githooks/pre-push"
GATES = ROOT / "tools/scripts/gates.sh"
WORKFLOW = ROOT / ".github/workflows/format-changed-check.yml"
SCRIPT = ROOT / "tools/scripts/format_changed.sh"

PINNED = "clang-format==21.1.8"

failures: list[str] = []


def fail(msg: str) -> None:
    failures.append(msg)
    print(f"FAIL: {msg}", file=sys.stderr)


def case_block(text: str, anchor: str, label: str) -> str:
    """The `case $? in ... esac` block that follows `anchor`."""
    start = text.find(anchor)
    if start < 0:
        fail(f"{label}: does not invoke format_changed.sh with `{anchor}`")
        return ""
    end = text.find("esac", start)
    if end < 0:
        fail(f"{label}: no `esac` after the format_changed invocation")
        return ""
    return text[start:end]


def branch(block: str, code: str) -> str:
    """The body of the `<code>)` arm of a case block."""
    m = re.search(rf"^\s*{re.escape(code)}\)(.*?)(?=^\s*(?:[0-9]+|\*)\)|\Z)", block, re.S | re.M)
    return m.group(1) if m else ""


def check_shell_caller(path: Path, anchor: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    block = case_block(text, anchor, label)
    if not block:
        return
    one = branch(block, "1")
    three = branch(block, "3")
    if not one or not three:
        fail(f"{label}: the case block must handle both exit 1 and exit 3 explicitly")
        return
    if "fail=1" in three:
        fail(f"{label}: exit 3 (no clang-format) sets fail=1 — a missing binary is reported as a formatting failure")
    if "INFRASTRUCTURE" not in three:
        fail(f"{label}: exit 3 must say INFRASTRUCTURE so nobody reads it as a verdict")
    if '"${PULP_ENFORCE_PREPUSH_FORMAT:-1}" = "1"' not in one:
        fail(f"{label}: exit 1 must block by default, demoted only by PULP_ENFORCE_PREPUSH_FORMAT=0")
    if "fail=1" not in one:
        fail(f"{label}: exit 1 never sets fail=1, so the gate cannot block")
    if "tools/scripts/format_changed.sh" not in one:
        fail(f"{label}: the exit 1 message must name the fix command")


def gate_block(text: str) -> str:
    """The caller's `if [ -f "$FMT" ]; then ... fi` block, verbatim."""
    start = text.find('if [ -f "$FMT" ]; then')
    end = text.find("\nfi\n", start)
    return text[start:end + 4] if start >= 0 and end >= 0 else ""


def run_block(block: str, stub_exit: int, env_value: str | None) -> int:
    """Run a caller's block with format_changed.sh replaced by a stub; return $fail."""
    with tempfile.TemporaryDirectory() as td:
        stub = Path(td) / "format_changed.sh"
        stub.write_text(f"#!/bin/sh\nexit {stub_exit}\n", encoding="utf-8")
        script = ("fail=0\nBASE=origin/main\n"
                  'run_gate_captured() { "$@"; }\n'
                  f'FMT="{stub}"\n{block}\necho "fail=$fail"\n')
        env = {k: v for k, v in os.environ.items() if k != "PULP_ENFORCE_PREPUSH_FORMAT"}
        if env_value is not None:
            env["PULP_ENFORCE_PREPUSH_FORMAT"] = env_value
        out = subprocess.run(["bash", "-c", script], env=env, capture_output=True, text=True,
                             timeout=30, encoding="utf-8").stdout
        match = re.search(r"fail=(\d)", out)
        return int(match.group(1)) if match else -1


def check_shell_behaviour(path: Path, label: str) -> None:
    block = gate_block(path.read_text(encoding="utf-8"))
    if not block:
        fail(f"{label}: no `if [ -f \"$FMT\" ]; then ... fi` block to run")
        return
    expected = {
        (0, None): 0,   # clean touched lines
        (1, None): 1,   # unformatted touched line: blocks by default
        (1, "0"): 0,    # one-push demotion
        (1, "1"): 1,
        (3, None): 0,   # no clang-format 21: infrastructure, never a verdict
    }
    for (code, knob), want in expected.items():
        got = run_block(block, code, knob)
        if got != want:
            fail(f"{label}: format_changed exit {code} with PULP_ENFORCE_PREPUSH_FORMAT="
                 f"{knob if knob is not None else '(unset)'} gave fail={got}, expected {want}")


def check_workflow() -> None:
    text = WORKFLOW.read_text(encoding="utf-8")
    if PINNED not in text:
        fail(f"workflow: clang-format must be pinned to `{PINNED}` (measured byte-identical to the local 21 binaries)")
    if not re.search(r"^\s*runs-on:\s*ubuntu-latest\s*$", text, re.M):
        fail("workflow: must run GitHub-hosted (ubuntu-latest), never on the pool that serves the required macos gate")
    block = case_block(text, 'bash tools/scripts/format_changed.sh --check --base "$BASE_REF"', "workflow")
    if not block:
        return
    one = branch(block, "1")
    three = branch(block, "3")
    # The annotation is what a contributor reads on the PR; the step summary
    # is secondary. Pin the annotation's title, not merely the word somewhere.
    if "::error title=INFRASTRUCTURE::" not in three:
        fail("workflow: exit 3 must fail the job with `::error title=INFRASTRUCTURE::`")
    if "::error title=Formatting" in three:
        fail("workflow: exit 3 carries the formatting-verdict annotation")
    if "NOT a formatting verdict" not in three:
        fail("workflow: exit 3 annotation must say it is not a formatting verdict")
    if "::error title=Formatting" not in one:
        fail("workflow: exit 1 must be the formatting verdict")
    if "INFRASTRUCTURE" in one:
        fail("workflow: exit 1 must not be labelled INFRASTRUCTURE")


def check_script() -> None:
    text = SCRIPT.read_text(encoding="utf-8")
    if "INFRASTRUCTURE: no clang-format found" not in text:
        fail("format_changed.sh: the no-binary exit must be labelled INFRASTRUCTURE")


def main() -> int:
    for p in (PREPUSH, GATES, WORKFLOW, SCRIPT):
        if not p.exists():
            fail(f"missing: {p.relative_to(ROOT)}")
    if failures:
        return 1
    check_shell_caller(PREPUSH, 'run_gate_captured bash "$FMT" --check --base "$BASE"', "pre-push")
    check_shell_caller(GATES, 'bash "$FMT" --check --base "$BASE"', "gates.sh")
    check_shell_behaviour(PREPUSH, "pre-push")
    check_shell_behaviour(GATES, "gates.sh")
    check_workflow()
    check_script()
    if failures:
        print(f"prepush-format-gate-wiring: {len(failures)} failure(s)", file=sys.stderr)
        return 1
    print("prepush-format-gate-wiring: ok (pre-push and gates.sh block exit 1 by default; exit 3 stays infrastructure everywhere)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
