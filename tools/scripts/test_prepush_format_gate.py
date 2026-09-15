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
"""

from __future__ import annotations

import re
import sys
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
    if "PULP_ENFORCE_PREPUSH_FORMAT" not in one:
        fail(f"{label}: exit 1 must be advisory unless PULP_ENFORCE_PREPUSH_FORMAT=1 (open branches are not clean yet)")
    if "fail=1" not in one:
        fail(f"{label}: exit 1 never sets fail=1, so the promotion knob is inert")


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
    check_workflow()
    check_script()
    if failures:
        print(f"prepush-format-gate-wiring: {len(failures)} failure(s)", file=sys.stderr)
        return 1
    print("prepush-format-gate-wiring: ok (pre-push, gates.sh, workflow keep exit 3 apart from exit 1)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
