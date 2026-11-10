#!/usr/bin/env python3
"""Check that the pull-request fast test tier selects what it promises.

`build.yml` runs `ctest -L '^pr-fast$'` on every pull request head. A label
that silently selects nothing, or loses the checks the merge queue is most
often ejected on, would turn that step green while testing nothing. This
script asks ctest itself which tests carry the label in a configured build
tree and fails when:

- the selection is smaller than ``--min-count``;
- any test in ``REQUIRED_MEMBERS`` is missing from it;
- the workflow step no longer selects the label, no longer fails on an empty
  selection, or writes the merge's own JUnit report;
- a ``pr-fast-configure`` member (run between Configure and Build, then left
  out of the post-build tier) is not also in pr-fast, needs the build, or
  is one of the drift checks the pre-build step exists for and has gone
  missing; or the pre-build step no longer runs before the build, or the
  post-build tier drops those members without that step having passed.

Usage:
    pr_fast_tier_check.py --build-dir build [--ctest ctest] [--workflow path]
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

LABEL = "pr-fast"
CONFIGURE_LABEL = "pr-fast-configure"
NEEDS_BUILD_LABEL = "needs-build"

# Static-contract checks whose failures ejected merge-queue batches after the
# full suite moved off the pull request head. Dropping one from the tier must be
# a deliberate edit here, not a side effect of renaming a registration.
REQUIRED_MEMBERS = (
    "consumption-census-drift",
    "consumption-census-negative-contract",
    "skip-not-pass-lint",
    "inspector-protocol-registry-complete",
    "agent-capability-manifest-check",
    "catch-discover-timeout-guard",
    "pr-fast-tier-contract",
    "unregistered-tests-check",
)

# These checks are intentionally selected by declared inputs / affected-test
# selection. Keeping them out of pr-fast prevents a multi-second probe from
# silently becoming part of every pull-request head run.
NON_FAST_MEMBERS = (
    "wide-non-native-selftest",
    "sample-region-compat-baseline",
)

# The generated-manifest drift checks behind most red pull-request heads. They
# read only the checkout and the configure, so the macOS leg runs them before
# the build (CONFIGURE_STEP_NAME) and the post-build tier leaves them out.
REQUIRED_CONFIGURE_MEMBERS = (
    "script-test-inputs-drift",
    "consumption-census-drift",
    "consumption-census-negative-contract",
    "agent-capability-manifest-check",
)

DEFAULT_MIN_COUNT = 50
STEP_NAME = "Test fast deterministic tier (pull request head)"
CONFIGURE_STEP_NAME = "Test configure-only drift checks before the build (pull request head)"
BUILD_STEP_MARKER = "- name: Build\n"
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"


def labelled_tests(show_only_json: dict, label: str = LABEL) -> list[str]:
    """Return the names of tests in a `ctest --show-only=json-v1` document
    that carry `label` (the pr-fast label by default)."""
    names = []
    for test in show_only_json.get("tests", []):
        for prop in test.get("properties", []):
            if prop.get("name") != "LABELS":
                continue
            value = prop.get("value") or []
            labels = value if isinstance(value, list) else str(value).split(";")
            if label in labels:
                names.append(test["name"])
                break
    return names


def check_selection(names: list[str], *, min_count: int) -> list[str]:
    errors = []
    if len(names) < min_count:
        errors.append(
            f"the {LABEL} label selects {len(names)} test(s); expected at least "
            f"{min_count}"
        )
    missing = [name for name in REQUIRED_MEMBERS if name not in names]
    if missing:
        errors.append(f"required {LABEL} members are missing: {', '.join(missing)}")
    misplaced = [name for name in NON_FAST_MEMBERS if name in names]
    if misplaced:
        errors.append(f"non-subsecond tests must not be in {LABEL}: {', '.join(misplaced)}")
    return errors


def check_configure_selection(fast: list[str], configure: list[str],
                              needs_build: list[str]) -> list[str]:
    """The pre-build members: each is a pr-fast member (the post-build tier
    drops the label, so one outside pr-fast would run nowhere on a pull
    request head), none needs the build, and the drift checks are there."""
    errors = []
    outside = sorted(set(configure) - set(fast))
    if outside:
        errors.append(f"{CONFIGURE_LABEL} members are not in {LABEL}: {', '.join(outside)}")
    built = sorted(set(configure) & set(needs_build))
    if built:
        errors.append(
            f"{CONFIGURE_LABEL} members also carry {NEEDS_BUILD_LABEL} and cannot run "
            f"before the build: {', '.join(built)}"
        )
    missing = [name for name in REQUIRED_CONFIGURE_MEMBERS if name not in configure]
    if missing:
        errors.append(f"required {CONFIGURE_LABEL} members are missing: {', '.join(missing)}")
    return errors


def _code(block: str) -> str:
    return "\n".join(line.split(" #", 1)[0] for line in block.splitlines()
                     if not line.lstrip().startswith("#"))


def _step_block(text: str, step_name: str = STEP_NAME) -> str | None:
    """Return the YAML text of the single step named `step_name`.

    Parsed textually so the check runs on the interpreter CTest resolves,
    which does not necessarily carry PyYAML.
    """
    marker = f"- name: {step_name}\n"
    if text.count(marker) != 1:
        return None
    start = text.index(marker)
    indent = text.rfind("\n", 0, start) + 1
    prefix = text[indent:start]
    end = text.find("\n" + prefix + "- ", start)
    return text[start:] if end < 0 else text[start:end]


def check_workflow(text: str) -> list[str]:
    """Static shape of the workflow step that runs the tier."""
    block = _step_block(text)
    if block is None:
        return [f"expected exactly one '{STEP_NAME}' step"]
    code = _code(block)
    errors = []
    if f"-L '^{LABEL}$'" not in code:
        errors.append(f"the step does not select -L '^{LABEL}$'")
    if "--no-tests=error" not in code:
        errors.append("the step does not fail on an empty selection (--no-tests=error)")
    if "/ctest.junit.xml" in code:
        errors.append(
            "the step writes ctest.junit.xml, the full suite's report; the tier "
            "must never read as the merge's test evidence"
        )
    if "github.event_name == 'pull_request'" not in code:
        errors.append("the step is not scoped to pull_request")
    # The post-build tier may leave the pre-build members out only when the
    # pre-build step passed; otherwise (contract lane, step skipped) it runs
    # the whole tier.
    drops = f"|^{CONFIGURE_LABEL}$" in code
    guard = 'if [ "$PR_FAST_CONFIGURE_OUTCOME" = success ]; then'
    guarded = (guard in code and
               f"|^{CONFIGURE_LABEL}$" in code.split(guard, 1)[1].split("\n          fi", 1)[0])
    if drops and not guarded:
        errors.append(
            f"the step leaves out {CONFIGURE_LABEL} without checking that the "
            "pre-build step passed"
        )
    if drops and "steps.pr_fast_configure_tests.outcome" not in code:
        errors.append("the step does not read the pre-build step's outcome")
    return errors + check_configure_workflow(text, post_build_drops=drops)


def check_configure_workflow(text: str, *, post_build_drops: bool = True) -> list[str]:
    """Static shape of the pre-build step that runs the pr-fast-configure members."""
    block = _step_block(text, CONFIGURE_STEP_NAME)
    if block is None:
        if post_build_drops:
            return [f"expected exactly one '{CONFIGURE_STEP_NAME}' step"]
        return []
    code = _code(block)
    errors = []
    if f"-L '^{LABEL}$' -L '^{CONFIGURE_LABEL}$'" not in code:
        errors.append(
            f"the pre-build step does not select -L '^{LABEL}$' -L '^{CONFIGURE_LABEL}$'"
        )
    if "--no-tests=error" not in code:
        errors.append("the pre-build step does not fail on an empty selection (--no-tests=error)")
    if "/ctest.junit.xml" in code:
        errors.append("the pre-build step writes ctest.junit.xml, the full suite's report")
    if "id: pr_fast_configure_tests" not in code:
        errors.append("the pre-build step lost its id: pr_fast_configure_tests")
    if "github.event_name == 'pull_request'" not in code:
        errors.append("the pre-build step is not scoped to pull_request")
    if "runner.os != 'Windows'" not in code:
        # Windows lists Catch2 tests by running the binaries, which do not
        # exist yet before the build.
        errors.append("the pre-build step is not scoped away from Windows")
    if '-LE "$label_exclude"' not in code:
        errors.append("the pre-build step does not apply the gate's label exclusions")
    if "matrix.lane != 'contract'" not in code:
        errors.append("the pre-build step also runs on the contract lane, which runs the whole tier")
    # Position: the step must sit between this job's Configure and its Build,
    # immediately before the Build step it exists to short-circuit.
    start = text.index(f"- name: {CONFIGURE_STEP_NAME}\n")
    after = text[start + len(block):].lstrip()
    if not after.startswith(BUILD_STEP_MARKER):
        errors.append("the pre-build step is not the step immediately before Build")
    configure = text.rfind("- name: Configure\n", 0, start)
    if configure < 0 or text.find(BUILD_STEP_MARKER, configure, start) >= 0:
        errors.append("the pre-build step does not follow this job's Configure step")
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--workflow", default=str(DEFAULT_WORKFLOW))
    parser.add_argument("--min-count", type=int, default=DEFAULT_MIN_COUNT)
    args = parser.parse_args(argv)

    proc = subprocess.run(
        [args.ctest, "--test-dir", args.build_dir, "-N", "--show-only=json-v1"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        print(f"ERROR: ctest -N failed ({proc.returncode}): {proc.stderr.strip()}")
        return 1
    try:
        document = json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        print(f"ERROR: ctest --show-only=json-v1 was not JSON: {exc}")
        return 1
    # Control: an empty listing means the instrument is pointed at the wrong
    # build tree, not that the label is empty.
    registered = len(document.get("tests", []))
    if registered == 0:
        print(f"ERROR: no tests registered under {args.build_dir}")
        return 1

    names = labelled_tests(document)
    errors = check_selection(names, min_count=args.min_count)
    errors += check_configure_selection(
        names,
        labelled_tests(document, CONFIGURE_LABEL),
        labelled_tests(document, NEEDS_BUILD_LABEL),
    )
    errors += check_workflow(Path(args.workflow).read_text(encoding="utf-8"))
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1
    print(f"OK: {LABEL} selects {len(names)} of {registered} registered tests")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
