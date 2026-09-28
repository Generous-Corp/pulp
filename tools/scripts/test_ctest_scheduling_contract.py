#!/usr/bin/env python3
"""The longest tests start first, and tests that need the VM alone run first.

ctest's parallel phase ends when its last long test finishes, and a test that
needs every slot can only start when all of them are free. Left to registration
order, both shapes became a serial tail at the end of the required gate's test
step: the RUN_SERIAL browser suites and a full-width reservation ran alone for
minutes after every other test had finished, while the suite's longest test
started minutes in because nothing told ctest it was long.

The rows and classes live in ctest_scheduling_policy.py, which the source-level
contract in test_ci_throughput_workflows.py imports too. The contract, read
from an already-configured build:

  * Every test that launches a real Chrome holds the `browser` RESOURCE_LOCK,
    so two captures never share a VM's cores. Only the serial-first tests may
    also be RUN_SERIAL.
  * Each serial-first test is RUN_SERIAL and carries a COST above every other
    registered test, so ctest starts it while every slot is free instead of
    after the parallel phase drains.
  * Each test measured as long carries a COST, so ctest starts it early, and
    reserves few enough slots to start while other tests are running.

Absence is handled the way the measured-budget guard handles it. A row that
shares this guard's enclosing condition is registered whenever the guard runs,
so its absence is a stale row. A row registered under another condition names a
companion from that condition: companion absent means the condition is off in
this build and the row is skipped; companion present means the row is stale.
A platform-gated row with no reliable companion is marked optional, and the
guard refuses to report success unless enough rows were actually checked.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys

from ctest_scheduling_policy import (
    BROWSER_LOCK,
    BROWSER_TESTS,
    LONG_TESTS,
    MAX_LONG_TEST_PROCESSORS,
    SERIAL_FIRST_TESTS,
    consistency_errors,
    row,
)

MINIMUM_EXPECTED_TESTS = 100

# A row set that checks almost nothing reads as clean; demand real coverage.
MINIMUM_CHECKED_ROWS = 3


def properties(test):
    return {prop["name"]: prop["value"] for prop in test.get("properties", [])}


def resolve(registered, entry, failures, skipped):
    """Return the registered test for a row, or None after recording why."""
    test = registered.get(entry["name"])
    if test is not None:
        return test
    companion = entry["companion"]
    if entry["optional"] or (companion is not None and companion not in registered):
        skipped.append(f"{entry['name']}: not registered in this build")
        return None
    where = (
        "shares this guard's enclosing condition"
        if companion is None
        else f"companion {companion!r} IS registered"
    )
    failures.append(
        f"{entry['name']}: listed but not registered ({where}); the row names a "
        f"registration that no longer exists"
    )
    return None


def audit(tests, browser_rows, long_rows, serial_first_rows=()):
    """Return (failures, skipped, checked) for the rows against registrations."""
    registered = {test["name"]: test for test in tests}
    serial_first = {entry["name"] for entry in serial_first_rows}
    failures, skipped = [], []
    checked = 0
    for entry in browser_rows:
        test = resolve(registered, entry, failures, skipped)
        if test is None:
            continue
        checked += 1
        props = properties(test)
        if props.get("RUN_SERIAL") and entry["name"] not in serial_first:
            failures.append(
                f"{entry['name']}: RUN_SERIAL holds the whole machine; the "
                f"`{BROWSER_LOCK}` lock is the isolation it needs unless the "
                f"policy lists it as serial-first"
            )
        if BROWSER_LOCK not in (props.get("RESOURCE_LOCK") or []):
            failures.append(
                f"{entry['name']}: launches Chrome without RESOURCE_LOCK "
                f"{BROWSER_LOCK!r}, so it can overlap another capture"
            )
    others = [
        properties(test).get("COST") or 0
        for test in tests if test["name"] not in serial_first
    ]
    ceiling = max(others, default=0)
    for entry in serial_first_rows:
        test = resolve(registered, entry, failures, skipped)
        if test is None:
            continue
        checked += 1
        props = properties(test)
        if not props.get("RUN_SERIAL"):
            failures.append(
                f"{entry['name']}: serial-first but not RUN_SERIAL, so it shares "
                f"the VM with other tests"
            )
        cost = props.get("COST") or 0
        if cost <= ceiling:
            failures.append(
                f"{entry['name']}: COST={cost:g} is not above every other test's "
                f"(highest {ceiling:g}), so it can wait for all slots until the "
                f"parallel phase drains and run alone at the end"
            )
    for entry in long_rows:
        test = resolve(registered, entry, failures, skipped)
        if test is None:
            continue
        checked += 1
        props = properties(test)
        if not props.get("COST"):
            failures.append(
                f"{entry['name']}: a long test without COST starts wherever its "
                f"registration order puts it and can finish last"
            )
        processors = props.get("PROCESSORS") or 1
        if props.get("RUN_SERIAL") or processors > MAX_LONG_TEST_PROCESSORS:
            failures.append(
                f"{entry['name']}: reserves the whole machine (PROCESSORS="
                f"{processors}, RUN_SERIAL={bool(props.get('RUN_SERIAL'))}), so "
                f"it runs alone after the suite drains whatever its COST"
            )
    return failures, skipped, checked


def self_check():
    """Prove each rule can fail before trusting a pass."""
    policy = consistency_errors()
    if policy:
        raise SystemExit("scheduling policy contradicts itself:\n  " + "\n  ".join(policy))
    if len(consistency_errors(weighted={"x"}, browser=set(), long={"x"},
                              serial_first=set())) != 1:
        raise SystemExit("detector self-check failed: a weighted long test was not rejected")
    browser_rows = [
        row("serial-browser"),
        row("unlocked-browser"),
        row("first-browser"),
        row("gone", companion="present-friend"),
        row("gone-elsewhere", companion="absent-friend"),
    ]
    serial_rows = [row("first-browser"), row("first-cheap"), row("first-shared")]
    long_rows = [row("no-cost"), row("full-width"), row("good-long"), row("gated", optional=True)]
    planted = [
        {"name": "serial-browser", "properties": [
            {"name": "RUN_SERIAL", "value": True},
            {"name": "RESOURCE_LOCK", "value": [BROWSER_LOCK]}]},
        {"name": "unlocked-browser", "properties": [
            {"name": "RESOURCE_LOCK", "value": ["pulp_gpu"]}]},
        # Serial-first and correct: RUN_SERIAL, locked, the highest COST.
        {"name": "first-browser", "properties": [
            {"name": "RUN_SERIAL", "value": True},
            {"name": "RESOURCE_LOCK", "value": [BROWSER_LOCK]},
            {"name": "COST", "value": 1000.0}]},
        # Serial-first with a COST a long test outranks.
        {"name": "first-cheap", "properties": [
            {"name": "RUN_SERIAL", "value": True},
            {"name": "COST", "value": 40.0}]},
        # Serial-first without RUN_SERIAL.
        {"name": "first-shared", "properties": [{"name": "COST", "value": 900.0}]},
        {"name": "present-friend", "properties": []},
        {"name": "no-cost", "properties": [{"name": "PROCESSORS", "value": 2}]},
        {"name": "full-width", "properties": [
            {"name": "COST", "value": 50.0}, {"name": "PROCESSORS", "value": 8}]},
        {"name": "good-long", "properties": [
            {"name": "COST", "value": 50.0}, {"name": "PROCESSORS", "value": 2}]},
    ]
    failures, skipped, checked = audit(planted, browser_rows, long_rows, serial_rows)
    flagged = sorted(line.split(":")[0] for line in failures)
    expected = sorted([
        "serial-browser", "unlocked-browser", "gone", "first-cheap", "first-shared",
        "no-cost", "full-width",
    ])
    if flagged != expected:
        raise SystemExit(f"detector self-check failed: expected {expected}, got {flagged}")
    if sorted(line.split(":")[0] for line in skipped) != ["gated", "gone-elsewhere"]:
        raise SystemExit(f"detector self-check failed: unexpected skips {skipped}")
    if checked != 9:
        raise SystemExit(f"detector self-check failed: checked {checked} rows, expected 9")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    args = parser.parse_args()

    self_check()

    done = subprocess.run(
        ["ctest", "--test-dir", args.build_dir, "--show-only=json-v1"],
        capture_output=True, text=True, check=False,
    )
    if done.returncode != 0:
        print(f"could not read registrations from {args.build_dir}:", file=sys.stderr)
        print((done.stderr or done.stdout).strip(), file=sys.stderr)
        return 1
    tests = json.loads(done.stdout).get("tests", [])
    if len(tests) < MINIMUM_EXPECTED_TESTS:
        print(f"only {len(tests)} tests registered in {args.build_dir}; refusing to "
              f"report a clean result from a build this empty.", file=sys.stderr)
        return 1

    failures, skipped, checked = audit(
        tests, BROWSER_TESTS, LONG_TESTS, SERIAL_FIRST_TESTS)
    for line in skipped:
        print(f"skipped {line}")
    if failures:
        print(f"\n{len(failures)} scheduling contract violation(s):\n", file=sys.stderr)
        for line in failures:
            print(f"  {line}", file=sys.stderr)
        return 1
    if checked < MINIMUM_CHECKED_ROWS:
        print(f"only {checked} row(s) were registered here; refusing to report a clean "
              f"result that checked almost nothing.", file=sys.stderr)
        return 1
    print(f"{len(tests)} registrations; {checked} scheduling row(s) hold the contract.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
