#!/usr/bin/env python3
"""A gate that COULD NOT RUN must block the push, never pass it.

Every gate script here already fails closed: a missing or unreadable config
exits 2. The hook then threw that away — `*) echo "…: internal error"` printed a
line and fell through WITHOUT setting `fail`, so an unmeasurable gate was
indistinguishable from a clean one. A gate that could not find its config
reported SUCCESS, and a missing version bump rode to main: no tag, no release.

Two things are asserted, because either alone is weak:

* the STRUCTURE — every gate's catch-all branch routes through
  `gate_could_not_run` and sets `fail=1`. A source assertion is what catches a
  NEW gate added later with the old copy-pasted fall-open branch.
* the BEHAVIOUR — the helper actually blocks. A structural assertion alone
  would pass over a `gate_could_not_run` that returned success.

`format_changed` is the one deliberate exemption and is asserted as such rather
than ignored: that gate is advisory by contract (it has its own documented
`3) SKIPPED` branch for a machine with no clang-format), so its catch-all stays
advisory. Pinning it here means a future change to its status is a test failure
rather than a silent drift back to the old shape.
"""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PREPUSH = ROOT / ".githooks/pre-push"
HELPER = ROOT / ".githooks/lib/gate-output.sh"

# The one documented advisory gate; see the module docstring.
ADVISORY_EXEMPT = {"format_changed"}


def check_structure(failures: list[str]) -> int:
    """Every catch-all branch blocks, and the scan proves it matched something."""
    source = PREPUSH.read_text(encoding="utf-8")

    blocking = re.findall(
        r'^\s*\*\) gate_could_not_run "([\w-]+)" "\$gate_rc"; fail=1 ;;$',
        source, re.M,
    )
    # `[^"]+`, not `[\w-]+`: a gate whose label carries arguments, e.g.
    # "agent_capability_manifest --check", contains a space and slipped the
    # narrower class entirely — a fall-open branch this scan could not see,
    # which is the exact blind spot the file exists to close.
    fall_open = re.findall(
        r'^\s*\*\) echo "\[pre-push\] ([^"]+?): internal error', source, re.M
    )

    # Control, on the TOTAL of both halves. A regex that silently stopped
    # matching would report "no fall-open branches" over zero coverage — the
    # exact failure mode this file exists to prevent. Counting both halves
    # means a gate moving between them cannot fake the control.
    if len(blocking) + len(fall_open) < 20:
        failures.append(
            f"scan found only {len(blocking) + len(fall_open)} gate branches; "
            "the pattern has drifted from the hook and is measuring nothing"
        )
        return len(blocking)
    unexpected = sorted(set(fall_open) - ADVISORY_EXEMPT)
    if unexpected:
        failures.append(
            "these gates still fall OPEN on a 'could not run' exit code "
            f"(they print and continue without setting fail=1): {unexpected}. "
            "Route them through gate_could_not_run and set fail=1."
        )

    missing_exempt = sorted(ADVISORY_EXEMPT - set(fall_open))
    if missing_exempt:
        failures.append(
            f"expected advisory exemption(s) {missing_exempt} were not found; "
            "if a gate stopped being advisory, update ADVISORY_EXEMPT here"
        )

    # Each blocking branch needs its status captured before `case` consumes $?.
    captures = source.count("gate_rc=$?; case $gate_rc in")
    if captures != len(blocking):
        failures.append(
            f"{len(blocking)} blocking branches but {captures} gate_rc captures; "
            'a branch is reading a stale "$gate_rc"'
        )
    return len(blocking)


def check_behaviour(failures: list[str]) -> None:
    """The helper must actually block, for every 'cannot measure' code."""
    if "gate_could_not_run()" not in HELPER.read_text(encoding="utf-8"):
        failures.append("gate-output.sh does not define gate_could_not_run")
        return

    for gate_exit, expect_block in ((0, False), (1, True), (2, True), (127, True)):
        with tempfile.TemporaryDirectory(prefix="pulp-cannot-measure-") as td:
            script = Path(td) / "harness.sh"
            # Mirrors the hook's real shape verbatim.
            script.write_text(
                f'source "{HELPER}"\n'
                "fail=0\n"
                f"( exit {gate_exit} )\n"
                "gate_rc=$?; case $gate_rc in\n"
                "    0) ;;\n"
                "    1) fail=1 ;;\n"
                '    *) gate_could_not_run "stub_gate" "$gate_rc"; fail=1 ;;\n'
                "esac\n"
                'exit "$fail"\n',
                encoding="utf-8",
            )
            proc = subprocess.run(
                ["bash", str(script)], capture_output=True, text=True
            )
        blocked = proc.returncode != 0
        if blocked != expect_block:
            failures.append(
                f"a gate exiting {gate_exit} "
                f"{'blocked' if blocked else 'PASSED'} — expected "
                f"{'block' if expect_block else 'pass'}"
            )
        if gate_exit not in (0, 1) and "COULD NOT RUN" not in proc.stderr:
            failures.append(
                f"a gate exiting {gate_exit} did not say it could not run; "
                f"stderr was: {proc.stderr!r}"
            )


def main() -> int:
    failures: list[str] = []
    if not PREPUSH.is_file():
        print(f"FAIL: {PREPUSH} not found", file=sys.stderr)
        return 1
    matched = check_structure(failures)
    check_behaviour(failures)

    if failures:
        for item in failures:
            print(f"FAIL: {item}", file=sys.stderr)
        return 1
    print(
        f"pre-push cannot-measure contract OK "
        f"({matched} gates block on an unmeasurable exit; "
        f"{len(ADVISORY_EXEMPT)} documented advisory exemption)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
