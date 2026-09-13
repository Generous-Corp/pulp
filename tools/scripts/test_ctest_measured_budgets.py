#!/usr/bin/env python3
"""A test that costs minutes may not run on the suite-wide default budget.

A registration with no explicit TIMEOUT inherits the lane's `ctest --timeout`,
which is 120s on every CI leg. That is invisible in the registration itself --
the absence of a property looks like nothing at all -- so a test whose real cost
grows into minutes keeps a budget nobody chose for it, and the margin it is
running on appears in no source file.

Four registrations reached that state together. On the required macOS gate the
slowest ran 67.75s against the 120s default: 1.77x headroom on a machine already
running the suite eight ways concurrently. Nothing was red, because headroom is
not a failure until a slower run consumes it -- and the run that consumes it
ejects a PR from the merge queue for a reason that has nothing to do with the PR.

The sibling detector next to this one groups a command's registrations and
rejects two different budgets for one command. Its scope note is explicit that it
"says nothing about whether a budget is the right size, only that one command is
not subject to two of them." This is the other half: whether the one budget in
force is large enough for what the test actually does.

Sizing is measured, not guessed. Each row records the worst run observed, where
it was observed, and when, and the budget must be at least MARGIN times that. The
margin is what absorbs a loaded host; the measurement is what makes the margin
mean something. A row whose test has since grown past its recorded cost fails
here rather than on a gate, which is the point.

Absence is the trap this has to handle carefully, because a manifest that is not
part of a given configure registers nothing and an empty result reads as clean.
Two absences are distinguishable:

  * A row whose registration shares an enclosing condition with THIS guard is
    registered whenever this guard runs. It carries no companion, and its absence
    can only mean the row names a registration that no longer exists.
  * A row from another manifest names a companion registered in that same
    manifest. Companion absent means the manifest is not in this build and the
    row is skipped; companion present means the manifest is here and the row is
    stale.

Scope: this reads an already-configured build, so it sees what that build would
run. It checks the declared budget against a recorded measurement -- it does not
re-measure, and it cannot see a test that has grown since its row was written
until that growth is measured and the row updated.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys

# A configure that registers almost nothing means the query failed or pointed at
# the wrong directory. Without a floor, that reads as a clean "no findings".
MINIMUM_EXPECTED_TESTS = 100

# How much room over the worst observed run a budget must leave. The gate runs
# the suite eight ways concurrently, so a test's wall time there is set as much
# by what it is co-scheduled with as by its own work.
MARGIN = 3.0


def budget(name, seconds, measured, source, companion=None):
    return {
        "name": name,
        "seconds": seconds,
        "measured": measured,
        "source": source,
        "companion": companion,
    }


# Worst run observed per test, with where and when. `companion=None` marks a row
# whose registration shares this guard's enclosing `if(Python3_Interpreter_FOUND)`
# in test/cmake/quality_tests.cmake: if this guard is running, that row's test is
# registered, so its absence is a stale row and needs no companion to prove it.
MEASUREMENTS = (
    budget(
        name="gpu-trace-overhead-verifier-selftest",
        seconds=90.94,
        measured="2026-09-13",
        source="90.94s locally at load 20 on 28 cores; 67.75s on the required macOS gate",
    ),
    budget(
        name="gpu-health-run-attestation",
        seconds=55.59,
        measured="2026-09-13",
        source="55.59s on the required macOS gate; 45.49s locally at load 20 on 28 cores",
        # Registered in test/cmake/gpu_health_tests.cmake, which is included
        # through an owner hub rather than directly, so it is genuinely absent
        # from some configures.
        companion="gpu-health-result",
    ),
    budget(
        name="version-at-land-selftest",
        seconds=39.06,
        measured="2026-09-13",
        source="39.06s on the required macOS gate; 24.10s locally at load 20 on 28 cores",
    ),
    budget(
        name="gpu-trace-overhead-acceptance-selftest",
        seconds=36.13,
        measured="2026-09-13",
        source="36.13s on the required macOS gate; 28.08s locally at load 20 on 28 cores",
    ),
)


def timeout_of(test):
    for prop in test.get("properties", []):
        if prop["name"] == "TIMEOUT":
            return prop["value"]
    return None


def audit(tests, inventory, margin):
    """Return (failures, skipped) for an inventory against registered tests.

    Parameterized by inventory so the self-check can exercise it on planted
    registrations rather than on the live rows.
    """
    registered = {test["name"]: test for test in tests}
    failures = []
    skipped = []
    for row in inventory:
        required = margin * row["seconds"]
        test = registered.get(row["name"])
        if test is None:
            companion = row["companion"]
            if companion is not None and companion not in registered:
                skipped.append(
                    f"{row['name']}: not in this build (companion {companion} "
                    f"absent too, so its manifest is not configured here)"
                )
                continue
            where = (
                "shares this guard's enclosing condition, so it is registered "
                "whenever this guard runs"
                if companion is None
                else f"companion {companion} IS registered, so its manifest is here"
            )
            failures.append(
                f"{row['name']}: inventoried but not registered -- {where}. "
                f"The row names a registration that no longer exists; rename or "
                f"remove it."
            )
            continue
        declared = timeout_of(test)
        if declared is None:
            failures.append(
                f"{row['name']}: no explicit TIMEOUT, so it inherits the lane's "
                f"--timeout (120s on every CI leg). Measured {row['seconds']}s "
                f"({row['source']}, {row['measured']}); needs at least "
                f"{required:.0f}s."
            )
        elif declared < required:
            failures.append(
                f"{row['name']}: TIMEOUT={declared:g}s is under {margin:g}x the "
                f"measured {row['seconds']}s ({row['source']}, {row['measured']}); "
                f"needs at least {required:.0f}s."
            )
    return failures, skipped


def self_check():
    """Prove the detector can fail before trusting it to pass.

    A scan that reports nothing is ambiguous between "no findings" and "the
    detector never looked". These cases fix that on both axes it decides: the
    budget comparison, and the two readings of an absent registration.
    """
    inventory = [
        budget("bare", 40.0, "2026-01-01", "planted"),
        budget("tight", 40.0, "2026-01-01", "planted"),
        budget("ample", 40.0, "2026-01-01", "planted"),
        budget("gone-manifest-here", 40.0, "2026-01-01", "planted", companion="present-friend"),
        budget("gone-manifest-absent", 40.0, "2026-01-01", "planted", companion="absent-friend"),
        budget("gone-co-registered", 40.0, "2026-01-01", "planted"),
    ]
    planted = [
        # Inventoried, no budget at all: the defect this exists for.
        {"name": "bare", "properties": []},
        # Inventoried, budget under the margin.
        {"name": "tight", "properties": [{"name": "TIMEOUT", "value": 100.0}]},
        # Inventoried, budget exactly at the margin: must pass, not trip a
        # strict comparison.
        {"name": "ample", "properties": [{"name": "TIMEOUT", "value": 120.0}]},
        # Not inventoried and bare: out of scope, must be ignored rather than
        # swept in as a finding.
        {"name": "stranger", "properties": []},
        # The companion that proves a manifest IS configured here.
        {"name": "present-friend", "properties": [{"name": "TIMEOUT", "value": 600.0}]},
    ]
    failures, skipped = audit(planted, inventory, MARGIN)
    flagged = sorted(line.split(":")[0] for line in failures)
    expected = ["bare", "gone-co-registered", "gone-manifest-here", "tight"]
    if flagged != expected:
        raise SystemExit(
            f"detector self-check failed: expected to flag {expected}, got {flagged}"
        )
    passed_over = sorted(line.split(":")[0] for line in skipped)
    if passed_over != ["gone-manifest-absent"]:
        raise SystemExit(
            f"detector self-check failed: expected to skip only "
            f"gone-manifest-absent, got {passed_over}"
        )


def main():
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

    failures, skipped = audit(tests, MEASUREMENTS, MARGIN)
    for line in skipped:
        print(f"skipped {line}")
    if failures:
        print(
            f"\n{len(failures)} measured test(s) without a budget sized to the "
            f"work.\nA test with no explicit TIMEOUT runs on the lane default, "
            f"and the run that finally exceeds it ejects a PR from the merge "
            f"queue:\n",
            file=sys.stderr,
        )
        for line in failures:
            print(f"  {line}", file=sys.stderr)
        print(
            "\nRe-measure the test, then set TIMEOUT to at least "
            f"{MARGIN:g}x that and update its row here with the new number and "
            "date.",
            file=sys.stderr,
        )
        return 1

    checked = len(MEASUREMENTS) - len(skipped)
    print(
        f"{len(tests)} registrations; {checked} measured test(s) carry a budget "
        f"of at least {MARGIN:g}x their measured cost."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
