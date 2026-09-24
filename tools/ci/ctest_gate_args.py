#!/usr/bin/env python3
"""Decide the event-dependent ctest arguments for the Build and Test workflow.

Two ctest decisions depend on which event is running the suite, and both were
previously inline shell in `.github/workflows/build.yml`. They live here so the
rules are stated once, tested, and cannot drift between the lanes that share
them.

The lanes, and why they differ:

* ``merge_group`` is a GATE. Its only question is "may this batch land?", and a
  batch that has one failing test is already unlandable. Running the remaining
  suite proves nothing the first failure did not, and a merge queue multiplies
  the waste: every batch inherits the break, every lane pays it, and each
  re-formed batch pays it again. So the queue stops at the first failure.

* ``push`` to main is the DETECTOR and the diagnostic. It is the only lane that
  runs the whole macOS suite on a commit that is actually on main, so it must
  report every failing test rather than the first one. Stopping early here
  would trade the one complete signal in the system for a few minutes.

* The gate events also exclude ``source-selftest``: registrations that read
  only the checkout, which the build-free required ``Enforce version & skill
  sync`` context runs instead (``tools/ci/source_selftests.py``).

* ``pull_request`` heads do not run the suite at all (the workflow's own Test
  step excludes them), so no rule here applies to them beyond the label set
  used by the dispatch lane that stands in for them.

The label set is a separate axis: `performance`, `bench` and `quality-lab` are
relative-timing tests that are robust to steady load but not to the load
VARIANCE of a host running concurrent build VMs. They are excluded wherever the
suite runs on the shared self-hosted macOS gate hosts, which now includes the
push lane, and retained on the steady GitHub-hosted Linux and Windows runners.
"""
from __future__ import annotations

import argparse
import json
import shlex
import sys

# Label set for lanes whose timing tests would be measuring runner load.
SHARED_HOST_LABEL_EXCLUDE = "validation|slow|performance|bench|quality-lab"
# Source-only Python selftests (tools/ci/source_selftests.json). The required
# `Enforce version & skill sync` context runs them without a build, so the gate
# events drop them; `push` keeps them as the macOS detector.
SOURCE_SELFTEST_LABEL = "source-selftest"
# Label set for the gate events.
GATE_LABEL_EXCLUDE = f"{SHARED_HOST_LABEL_EXCLUDE}|{SOURCE_SELFTEST_LABEL}"
# Label set for steady-load lanes, which keep the heavier informational tests.
FULL_LABEL_EXCLUDE = "validation"

# Events whose suite runs on the shared self-hosted macOS gate hosts, or which
# stand in for them.
GATE_EVENTS = frozenset({"pull_request", "workflow_dispatch", "merge_group"})

STOP_ON_FAILURE_FLAG = "--stop-on-failure"


def _norm(value: str | None) -> str:
    return (value or "").strip()


def label_exclude(event_name: str, runner_os: str) -> str:
    """The ctest ``-LE`` value for this lane."""
    event = _norm(event_name)
    if event in GATE_EVENTS:
        return GATE_LABEL_EXCLUDE
    # A push builds macOS on the same shared Studios that serve the required
    # gate, so it inherits the gate's timing-test exclusions. Linux and Windows
    # pushes stay on GitHub-hosted runners under steady load and keep the
    # heavier set.
    if event == "push" and _norm(runner_os).lower() == "macos":
        return SHARED_HOST_LABEL_EXCLUDE
    return FULL_LABEL_EXCLUDE


def stop_on_failure(event_name: str) -> str:
    """``--stop-on-failure`` for the gate lane, empty for every other lane.

    ctest applies this only after ``--repeat until-pass`` has exhausted its
    retries, so a single flake still self-heals; only a test that fails every
    attempt stops the run.
    """
    return STOP_ON_FAILURE_FLAG if _norm(event_name) == "merge_group" else ""


def decide(event_name: str, runner_os: str) -> dict[str, str]:
    return {
        "label_exclude": label_exclude(event_name, runner_os),
        "stop_on_failure": stop_on_failure(event_name),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event-name", required=True)
    parser.add_argument("--runner-os", default="")
    parser.add_argument("--format", choices=("shell", "json"), default="shell")
    args = parser.parse_args(argv)

    decision = decide(args.event_name, args.runner_os)
    if args.format == "json":
        print(json.dumps(decision, sort_keys=True))
        return 0
    # Shell-quoted so the caller can `eval` the output without word-splitting
    # surprises, even though every value in the vocabulary is already a single
    # shell-safe token.
    for key, value in decision.items():
        print(f"{key}={shlex.quote(value)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
