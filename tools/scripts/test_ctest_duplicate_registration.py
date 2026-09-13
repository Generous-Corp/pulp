#!/usr/bin/env python3
"""Two registrations of one command may not carry different TIMEOUT budgets.

Registering the same command twice is not itself a defect here: a second
`catch_discover_tests(... TEST_SPEC "[lifecycle]" TEST_PREFIX "lifecycle::"
PROPERTIES LABELS lifecycle)` deliberately re-registers a tagged subset so a lane
can select it by label, and 32 pairs in the tree do exactly that. They share one
budget, so whichever name runs, the same limit applies.

The defect is a command registered twice under DIFFERENT budgets. Both run on
every lane that runs the suite, and the tighter one decides whether the required
gate is green -- so a test that comfortably passes under its intended budget can
still eject a PR from the merge queue under a budget nobody meant to give it.
That is not hypothetical: `tools/rack/test_generate_safety.py` passed at 136.68s
under TIMEOUT 180 while timing out at 60.11s under TIMEOUT 60, in the same run,
on the same host -- the in-run control that proves it was the budget and not the
machine.

It survives review because a CMake `foreach` with `string(REPLACE ...)` assembles
the registered name from parts, so the full name exists nowhere in the tree as a
literal string. Someone adding a second registration greps for the name they are
about to use, finds nothing, and adds a duplicate in good faith.

Grouping is by command AND environment AND working directory, because a suite
that runs one binary under several environments is a legitimate and common
pattern: `cli-doctor-host-quirks{,-off,-validated}` share an argv and differ only
in `PULP_HOST_QUIRKS`. Those are three distinct tests, not one command twice.

A registration with no explicit TIMEOUT counts as its own budget, because it
inherits the lane's `ctest --timeout` -- which is a different number from any
explicit one, and the same trap.

Scope: this reads the registrations of an already-configured build, so it sees
exactly what that build would run, including names no literal grep can find. It
says nothing about whether a budget is the right size, only that one command is
not subject to two of them.
"""

from __future__ import annotations

import argparse
import collections
import json
import subprocess
import sys

# A configure that registers almost nothing means the query failed or pointed at
# the wrong directory. Without a floor, that reads as a clean "no findings".
MINIMUM_EXPECTED_TESTS = 100


def registration_key(test: dict) -> tuple:
    props = {p["name"]: p["value"] for p in test.get("properties", [])}
    env = props.get("ENVIRONMENT") or []
    return (
        tuple(test.get("command", [])),
        tuple(sorted(env)),
        props.get("WORKING_DIRECTORY") or "",
    )


def timeout_of(test: dict) -> float | None:
    for prop in test.get("properties", []):
        if prop["name"] == "TIMEOUT":
            return prop["value"]
    return None


def divergent_budgets(tests: list[dict]) -> dict[tuple, list[tuple[str, float | None]]]:
    by_key = collections.defaultdict(list)
    for test in tests:
        if test.get("command"):
            by_key[registration_key(test)].append((test["name"], timeout_of(test)))
    return {
        key: sorted(rows)
        for key, rows in by_key.items()
        if len({timeout for _, timeout in rows}) > 1
    }


def self_check() -> None:
    """Prove the detector can fail before trusting it to pass.

    A scan that reports nothing is ambiguous between "no findings" and "the
    detector never looked". These cases fix that: one divergent pair it must
    catch, one equal-budget pair it must leave alone (the `lifecycle::` idiom),
    and one environment-differing pair it must not group in the first place.
    """
    planted = [
        {
            "name": "alpha",
            "command": ["python3", "run.py"],
            "properties": [{"name": "TIMEOUT", "value": 60.0}],
        },
        {
            "name": "beta",
            "command": ["python3", "run.py"],
            "properties": [{"name": "TIMEOUT", "value": 180.0}],
        },
        {
            "name": "shared",
            "command": ["suite", "a case"],
            "properties": [{"name": "TIMEOUT", "value": 30.0}],
        },
        {
            "name": "lifecycle::shared",
            "command": ["suite", "a case"],
            "properties": [
                {"name": "TIMEOUT", "value": 30.0},
                {"name": "LABELS", "value": ["lifecycle"]},
            ],
        },
        {
            "name": "gamma",
            "command": ["tool", "--check"],
            "properties": [
                {"name": "ENVIRONMENT", "value": ["MODE=on"]},
                {"name": "TIMEOUT", "value": 10.0},
            ],
        },
        {
            "name": "delta",
            "command": ["tool", "--check"],
            "properties": [
                {"name": "ENVIRONMENT", "value": ["MODE=off"]},
                {"name": "TIMEOUT", "value": 900.0},
            ],
        },
    ]
    found = divergent_budgets(planted)
    names = sorted(
        [name for name, _ in rows] for rows in found.values()
    )
    if names != [["alpha", "beta"]]:
        raise SystemExit(
            f"detector self-check failed: expected exactly the alpha/beta "
            f"divergence, got {names}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    args = parser.parse_args()

    self_check()

    done = subprocess.run(
        ["ctest", "--test-dir", args.build_dir, "--show-only=json-v1"],
        capture_output=True,
        text=True,
        check=False,
    )
    if done.returncode != 0:
        print(f"could not read registrations from {args.build_dir}:", file=sys.stderr)
        print((done.stderr or done.stdout).strip(), file=sys.stderr)
        return 1

    tests = json.loads(done.stdout).get("tests", [])
    if len(tests) < MINIMUM_EXPECTED_TESTS:
        print(
            f"only {len(tests)} tests registered in {args.build_dir}; expected at "
            f"least {MINIMUM_EXPECTED_TESTS}. Refusing to report a clean result "
            f"from a build this empty.",
            file=sys.stderr,
        )
        return 1

    found = divergent_budgets(tests)
    if found:
        print(
            f"{len(found)} command(s) registered under more than one TIMEOUT "
            f"budget.\nBoth run every suite, and the tighter budget is the one "
            f"that decides the gate:\n",
            file=sys.stderr,
        )
        for key, rows in sorted(found.items(), key=lambda kv: kv[1]):
            for name, timeout in rows:
                shown = "none (inherits the lane's --timeout)" if timeout is None else timeout
                print(f"  {name}  TIMEOUT={shown}", file=sys.stderr)
            print(f"      {' '.join(str(part) for part in key[0])}", file=sys.stderr)
        print(
            "\nKeep the registration the changed-surface policy names and remove "
            "the other, or give both the same budget.",
            file=sys.stderr,
        )
        return 1

    print(f"{len(tests)} registrations, no command under two budgets.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
