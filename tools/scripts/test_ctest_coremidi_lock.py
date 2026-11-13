#!/usr/bin/env python3
"""Every test that creates or opens real CoreMIDI endpoints holds one lock.

CoreMIDI endpoints are server-global: a suite listing endpoints sees the ones
another suite is creating at that moment. Under parallel ctest the suites race
and fail at random, so each of them carries RESOURCE_LOCK pulp_coremidi. The
contract is read from an already-configured build with `ctest --show-only`.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys

LOCK = "pulp_coremidi"
# Executables whose cases drive the real CoreMIDI server, matched on the
# registered test's command (the discovered test names are Catch2 case names).
COREMIDI_EXECUTABLES = (
    "pulp-test-midi-output-sysex",
    "pulp-test-midi-port-identity",
    "pulp-test-coremidi-shared-client",
    "pulp-test-coremidi-reconnect",
)
MINIMUM_EXPECTED_TESTS = 100


def audit(tests):
    """Return (failures, checked) for registrations of the CoreMIDI executables."""
    failures, checked = [], 0
    for test in tests:
        command = " ".join(test.get("command") or [])
        exe = next((e for e in COREMIDI_EXECUTABLES
                    if "/" + e + " " in command + " " or command.endswith("/" + e)), None)
        if exe is None:
            continue
        checked += 1
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        locks = props.get("RESOURCE_LOCK") or []
        if isinstance(locks, str):
            locks = [locks]
        if LOCK not in locks:
            failures.append(f"{test['name']} ({exe}): RESOURCE_LOCK is {locks!r}, "
                            f"expected to include {LOCK}")
    return failures, checked


def self_check():
    def make(exe, locks):
        props = [{"name": "RESOURCE_LOCK", "value": locks}] if locks is not None else []
        return {"name": exe, "command": [f"/b/test/{exe}", "x"], "properties": props}
    bad, checked = audit([make("pulp-test-midi-port-identity", None),
                          make("pulp-test-midi-output-sysex", ["other"]),
                          make("pulp-test-coremidi-reconnect", [LOCK]),
                          make("pulp-test-unrelated", None)])
    if len(bad) != 2 or checked != 3:
        raise SystemExit(f"detector self-check failed: {bad} checked={checked}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    args = parser.parse_args()
    self_check()
    done = subprocess.run(
        ["ctest", "--test-dir", args.build_dir, "--show-only=json-v1"],
        capture_output=True, text=True, check=False)
    if done.returncode != 0:
        print((done.stderr or done.stdout).strip(), file=sys.stderr)
        return 1
    tests = json.loads(done.stdout).get("tests", [])
    if len(tests) < MINIMUM_EXPECTED_TESTS:
        print(f"only {len(tests)} tests registered; refusing a clean result "
              f"from a build this empty.", file=sys.stderr)
        return 1
    failures, checked = audit(tests)
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    if sys.platform == "darwin" and checked == 0:
        print("no CoreMIDI tests registered on macOS; the contract checked nothing.",
              file=sys.stderr)
        return 1
    print(f"{checked} CoreMIDI registration(s) hold {LOCK}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
