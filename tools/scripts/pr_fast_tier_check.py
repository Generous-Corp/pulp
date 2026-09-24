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
  selection, or writes the merge's own JUnit report.

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
)

DEFAULT_MIN_COUNT = 50
STEP_NAME = "Test fast deterministic tier (pull request head)"
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"


def labelled_tests(show_only_json: dict) -> list[str]:
    """Return the names of tests in a `ctest --show-only=json-v1` document
    that carry the pr-fast label."""
    names = []
    for test in show_only_json.get("tests", []):
        for prop in test.get("properties", []):
            if prop.get("name") != "LABELS":
                continue
            value = prop.get("value") or []
            labels = value if isinstance(value, list) else str(value).split(";")
            if LABEL in labels:
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
    return errors


def _step_block(text: str) -> str | None:
    """Return the YAML text of the single step named STEP_NAME.

    Parsed textually so the check runs on the interpreter CTest resolves,
    which does not necessarily carry PyYAML.
    """
    marker = f"- name: {STEP_NAME}\n"
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
    code = "\n".join(line.split(" #", 1)[0] for line in block.splitlines()
                     if not line.lstrip().startswith("#"))
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
    errors += check_workflow(Path(args.workflow).read_text(encoding="utf-8"))
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1
    print(f"OK: {LABEL} selects {len(names)} of {registered} registered tests")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
