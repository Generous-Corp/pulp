#!/usr/bin/env python3
"""The longest tests start first, and no Chrome-launching test holds the machine.

ctest's parallel phase ends when its last long test finishes, and anything that
reserves every slot can only start once the rest of the suite has drained. Both
shapes turned into a serial tail at the end of the required gate's test step: a
full-width reservation and a RUN_SERIAL browser suite ran alone, one after the
other, for minutes after every other test had finished, while the suite's
longest test started minutes in because nothing told ctest it was long.

The contract, read from an already-configured build:

  * Every test that launches a real Chrome holds the `browser` RESOURCE_LOCK,
    so two captures never share a VM's cores, and none is RUN_SERIAL: the lock
    is the isolation it needs, and RUN_SERIAL excludes every other test too.
  * The real-browser suite's PROCESSORS equals the reservation it hands its
    launcher (PULP_BROWSER_CAPTURE_RESERVED_CORES), so the browsers it starts
    fit the slots ctest set aside for them.
  * Each test measured as long carries a COST, so ctest starts it first, and
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

MINIMUM_EXPECTED_TESTS = 100

# A row set that checks almost nothing reads as clean; demand real coverage.
MINIMUM_CHECKED_ROWS = 3

BROWSER_LOCK = "browser"
RESERVATION_ENV = "PULP_BROWSER_CAPTURE_RESERVED_CORES"
BROWSER_SUITE = "pulp-browser-capture-node-integration"

# The smallest gate VM runs ctest at -j3. A long test reserving more than this
# many slots cannot start until most of the suite is idle on any host.
MAX_LONG_TEST_PROCESSORS = 4


def row(name, companion=None, optional=False):
    return {"name": name, "companion": companion, "optional": optional}


# Tests that launch a real Chrome (directly, or through the importer).
BROWSER_TESTS = (
    row(BROWSER_SUITE, companion="pulp-browser-capture-node-unit"),
    row("pulp-browser-capture-process-lifecycle", companion="pulp-browser-capture-node-unit"),
    row("generic browser HTML supplies the reference for --fail-below",
        companion="pulp-import-design reports help and argument diagnostics"),
    row("agent-panel-native-invariants"),
    row("agent-panel-clipped-is-rejected"),
)

# Tests over 40s on the required gate's merge-group runs (2026-09-27, studio and
# m5 hosts), longest first.
LONG_TESTS = (
    row("sample-region-compat-baseline", optional=True),
    row(BROWSER_SUITE, companion="pulp-browser-capture-node-unit"),
    row("cmake-control-sdk-consumer", optional=True),
    row("gpu-trace-overhead-acceptance-selftest"),
    row("gpu-first-visible-role-producers-selftest"),
    row("combined-installer-selftest", optional=True),
)


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


def audit(tests, browser_rows, long_rows):
    """Return (failures, skipped, checked) for the rows against registrations."""
    registered = {test["name"]: test for test in tests}
    failures, skipped = [], []
    checked = 0
    for entry in browser_rows:
        test = resolve(registered, entry, failures, skipped)
        if test is None:
            continue
        checked += 1
        props = properties(test)
        if props.get("RUN_SERIAL"):
            failures.append(
                f"{entry['name']}: RUN_SERIAL holds the whole machine; the "
                f"`{BROWSER_LOCK}` lock is the isolation a Chrome-launching test needs"
            )
        if BROWSER_LOCK not in (props.get("RESOURCE_LOCK") or []):
            failures.append(
                f"{entry['name']}: launches Chrome without RESOURCE_LOCK "
                f"{BROWSER_LOCK!r}, so it can overlap another capture"
            )
        if entry["name"] == BROWSER_SUITE:
            reserved = None
            for assignment in props.get("ENVIRONMENT") or []:
                key, _, value = assignment.partition("=")
                if key == RESERVATION_ENV:
                    reserved = value
            processors = props.get("PROCESSORS")
            if reserved is None or processors is None or str(processors) != reserved:
                failures.append(
                    f"{entry['name']}: PROCESSORS={processors} and "
                    f"{RESERVATION_ENV}={reserved} must be set and equal, or the "
                    f"launcher starts browsers on slots ctest gave other tests"
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
    browser_rows = [
        row("serial-browser"),
        row("unlocked-browser"),
        row(BROWSER_SUITE),
        row("gone", companion="present-friend"),
        row("gone-elsewhere", companion="absent-friend"),
    ]
    long_rows = [row("no-cost"), row("full-width"), row("good-long"), row("gated", optional=True)]
    planted = [
        {"name": "serial-browser", "properties": [
            {"name": "RUN_SERIAL", "value": True},
            {"name": "RESOURCE_LOCK", "value": [BROWSER_LOCK]}]},
        {"name": "unlocked-browser", "properties": [
            {"name": "RESOURCE_LOCK", "value": ["pulp_gpu"]}]},
        {"name": BROWSER_SUITE, "properties": [
            {"name": "RESOURCE_LOCK", "value": [BROWSER_LOCK]},
            {"name": "PROCESSORS", "value": 4},
            {"name": "ENVIRONMENT", "value": [f"{RESERVATION_ENV}=3"]}]},
        {"name": "present-friend", "properties": []},
        {"name": "no-cost", "properties": [{"name": "PROCESSORS", "value": 2}]},
        {"name": "full-width", "properties": [
            {"name": "COST", "value": 50.0}, {"name": "PROCESSORS", "value": 8}]},
        {"name": "good-long", "properties": [
            {"name": "COST", "value": 50.0}, {"name": "PROCESSORS", "value": 2}]},
    ]
    failures, skipped, checked = audit(planted, browser_rows, long_rows)
    flagged = sorted(line.split(":")[0] for line in failures)
    expected = sorted([
        "serial-browser", "unlocked-browser", BROWSER_SUITE, "gone", "no-cost", "full-width",
    ])
    if flagged != expected:
        raise SystemExit(f"detector self-check failed: expected {expected}, got {flagged}")
    if sorted(line.split(":")[0] for line in skipped) != ["gated", "gone-elsewhere"]:
        raise SystemExit(f"detector self-check failed: unexpected skips {skipped}")
    if checked != 6:
        raise SystemExit(f"detector self-check failed: checked {checked} rows, expected 6")


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

    failures, skipped, checked = audit(tests, BROWSER_TESTS, LONG_TESTS)
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
