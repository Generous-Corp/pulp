#!/usr/bin/env python3
"""Contract: Android keeps a real pre-merge compile signal.

`android-build` runs only on push/schedule/dispatch, so `android-run-fixtures`
is the sole Android job a pull request or merge group executes. A core header
that uses a libc symbol above the app's API floor compiles fine in the fixture
runners' narrower target closure, so without an explicit API-floor probe that
class of break reaches `main` with every pre-merge signal green.

These assertions fail if the probe step, its gate, or the dependency closure
that reaches it is dropped.
"""

import pathlib
import sys

import yaml

WORKFLOW = pathlib.Path(__file__).resolve().parents[2] / ".github/workflows/android.yml"
PROBE = "tools/scripts/test_android_aligned_buffer.py"
JOB = "android-run-fixtures"
GATE = "steps.fixture-scope.outputs.affected == 'true'"


def check(workflow: dict) -> list[str]:
    failures: list[str] = []
    jobs = workflow.get("jobs", {})
    job = jobs.get(JOB)
    if job is None:
        return [f"{JOB} job is missing; nothing runs Android code pre-merge"]

    # The job must stay reachable from pull_request and merge_group. Sibling
    # jobs gate themselves off those events; this one may not.
    condition = str(job.get("if", ""))
    for event in ("pull_request", "merge_group"):
        if f"!= '{event}'" in condition or f'!= "{event}"' in condition:
            failures.append(f"{JOB} excludes {event}; no Android job would run pre-merge")

    steps = job.get("steps", [])
    # Match the invocation, not the bare path: the classifier step below
    # also names the probe, as one of the paths in its dependency closure.
    probe = [s for s in steps if f"python3 {PROBE}" in str(s.get("run", ""))]
    if not probe:
        failures.append(f"{JOB} has no step running {PROBE}")
    else:
        for step in probe:
            if str(step.get("if", "")).strip() != GATE:
                failures.append(f"{JOB} probe step must be gated on `{GATE}`")

    # The classifier's dependency closure must reach the probe. `core/*` is the
    # surface the probe exists to protect; the probe's own path keeps an edit to
    # it self-testing.
    scope = [s for s in steps if s.get("id") == "fixture-scope"]
    if not scope:
        failures.append(f"{JOB} has no `fixture-scope` classifier step")
    else:
        body = str(scope[0].get("run", ""))
        for needed in ("core/*", PROBE):
            if needed not in body:
                failures.append(f"fixture-scope closure does not admit `{needed}`")
    return failures


def main() -> int:
    failures = check(yaml.safe_load(WORKFLOW.read_text()))
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print(f"android pre-merge probe contract OK ({WORKFLOW.name})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
