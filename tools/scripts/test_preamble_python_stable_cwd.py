#!/usr/bin/env python3
"""Guard inline Python used by Build-and-Test preamble polling jobs.

The self-hosted preamble can check out Pulp below /Volumes/Workshop. Python's
stdin mode resolves the current working directory while computing sys.path[0];
if that volume wedges, Python can hang before it executes any of the supplied
script. Inline helpers in those jobs must therefore chdir to system /tmp first;
RUNNER_TEMP may live on the same work volume as the checkout.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"


def _assert(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _job_body(text: str, job_name: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(job_name)}:\n(?P<body>.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)",
        text,
    )
    _assert(match is not None, f"build.yml is missing job {job_name!r}")
    return match.group("body")


def _preamble_jobs(text: str) -> list[tuple[str, str]]:
    jobs = []
    for match in re.finditer(
        r"(?ms)^  (?P<name>[a-zA-Z0-9_-]+):\n"
        r"(?P<body>.*?)(?=^  [a-zA-Z0-9_-]+:\n|\Z)",
        text,
    ):
        body = match.group("body")
        if "PULP_PREAMBLE_RUNS_ON_JSON" in body:
            jobs.append((match.group("name"), body))
    return jobs


def test_all_preamble_inline_python_uses_stable_cwd() -> None:
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    protected_invocation = re.compile(
        r"cd /tmp\s*\n\s*python(?:3)? -(?:\s|\")"
    )

    inline_count = 0
    preamble_jobs = _preamble_jobs(text)
    _assert(preamble_jobs, "build.yml has no PULP_PREAMBLE_RUNS_ON_JSON jobs")
    for job_name, body in preamble_jobs:
        invocations = re.findall(r"(?m)^\s*python(?:3)? -(?:\s|$|\")", body)
        inline_count += len(invocations)
        protected = protected_invocation.findall(body)
        _assert(
            len(protected) == len(invocations),
            f"{job_name} has {len(invocations)} inline Python invocation(s), "
            f"but only {len(protected)} first chdir to system /tmp",
        )

    _assert(
        inline_count == 2,
        f"expected exactly 2 preamble inline Python invocations, found {inline_count}; "
        "classify new helpers explicitly",
    )


def test_resolver_uses_absolute_repo_script_after_chdir() -> None:
    body = _job_body(BUILD_WORKFLOW.read_text(encoding="utf-8"), "resolve-provider")
    _assert(
        "GITHUB_WORKSPACE" in body
        and re.search(
            r'WORKSPACE\s*/\s*"tools"\s*/\s*"scripts"\s*/\s*"resolve_runs_on\.py"',
            body,
        ) is not None,
        "resolve-provider must use GITHUB_WORKSPACE after leaving the checkout cwd",
    )


def main() -> int:
    tests = [
        value
        for name, value in globals().items()
        if name.startswith("test_") and callable(value)
    ]
    failures = 0
    for test in tests:
        try:
            test()
            print(f"ok  {test.__name__}")
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"FAIL {test.__name__}: {exc}")
    if failures:
        print(f"\n{failures} failing test(s)")
        return 1
    print(f"\nall {len(tests)} tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
