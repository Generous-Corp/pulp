#!/usr/bin/env python3
"""Run the tree-reading drift checks against a configured, unbuilt tree.

Most merge groups that fail the required ``macos`` gate fail only Python
registrations that read the checkout and the configured test graph: census,
script-inputs, tools-registry and lane-contract drift, and the
wide-non-native replay. They surface only when a head is combined with the
current tip, so a clean pull-request run does not predict them, and on the gate
they run after a ~20-minute build. None of them needs that build.

``tools/ci/drift_fast.json`` names them: whole ctest labels (``ctest_labels``)
plus individual registrations (``tests``). ``run`` resolves that selection
against a configured build tree and runs it with ctest, so SKIP_RETURN_CODE,
PASS_REGULAR_EXPRESSION, TIMEOUT and RESOURCE_LOCK behave exactly as on the
gate. The advisory hosted ``drift-fast`` job
(``.github/workflows/drift-fast.yml``) is its caller.

It reports, rather than hides, what it could not check: a listed test this
configuration did not register (an Apple-only registration on a Linux
configure) and a test that skipped. It fails, rather than passing vacuously,
when a label selects nothing or when ctest ran a different number of tests
than were selected.

``check`` is the static contract: the manifest parses, names no test twice,
the workflow still calls ``run`` on both pull_request and merge_group, and it
runs every ``workflow_preconditions`` step before that call.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "tools" / "ci" / "drift_fast.json"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "drift-fast.yml"
# The gate retries a failing test once; so does this lane, so a flake the gate
# tolerates cannot turn it red.
REPEAT = "until-pass:2"
DEFAULT_TIMEOUT = 300

# ctest's summary line: "92% tests passed, 1 tests failed out of 12"; recent
# ctest drops the failed clause when nothing failed ("100% tests passed out of
# 12"), and older ctest keeps it ("..., 0 tests failed out of 12").
SUMMARY = re.compile(r"tests passed(?:, \d+ tests? failed)? out of (\d+)")
SKIPPED = re.compile(r"Test\s+#\d+:\s+(\S+)\s+\.+\s*\*+Skipped")


def load_manifest(path: pathlib.Path = MANIFEST) -> dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1:
        raise SystemExit(f"drift-fast: {path}: unsupported schema_version")
    return data


def manifest_problems(data: dict[str, Any]) -> list[str]:
    problems: list[str] = []
    labels = data.get("ctest_labels", [])
    tests = data.get("tests", [])
    if not isinstance(labels, list) or not all(isinstance(x, str) and x for x in labels):
        problems.append("ctest_labels must be a list of non-empty strings")
    preconditions = data.get("workflow_preconditions", [])
    if not isinstance(preconditions, list) or not all(
        isinstance(x, dict) and x.get("step") and x.get("why") for x in preconditions
    ):
        problems.append("workflow_preconditions entries need a 'step' and a 'why'")
    if not isinstance(tests, list) or not tests:
        problems.append("tests must be a non-empty list")
        return problems
    seen: set[str] = set()
    for entry in tests:
        name = entry.get("name") if isinstance(entry, dict) else None
        if not isinstance(name, str) or not name:
            problems.append(f"entry without a name: {entry!r}")
            continue
        if not entry.get("why"):
            problems.append(f"{name}: missing 'why'")
        if name in seen:
            problems.append(f"{name}: listed twice")
        seen.add(name)
    return problems


def ctest_inventory(ctest: str, build_dir: pathlib.Path) -> dict[str, Any]:
    """ctest's own JSON description of the configured registrations."""
    proc = subprocess.run(
        [ctest, "--test-dir", str(build_dir), "--show-only=json-v1"],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise SystemExit(f"drift-fast: ctest --show-only failed:\n{proc.stderr}")
    return json.loads(proc.stdout)


def parse_registrations(doc: dict[str, Any]) -> dict[str, set[str]]:
    out: dict[str, set[str]] = {}
    for test in doc.get("tests", []):
        labels: set[str] = set()
        for prop in test.get("properties", []):
            if prop.get("name") == "LABELS":
                labels.update(prop.get("value") or [])
        out[test["name"]] = labels
    return out


def unbuilt_artifacts(doc: dict[str, Any], build_dir: pathlib.Path) -> dict[str, str]:
    """Tests whose command names a build-tree file that a configure does not create.

    Such a test needs the build this lane skips. Run without it, it fails for
    the configuration, or worse passes vacuously: a WILL_FAIL mutation control
    whose runner cannot find its binary "fails as expected" without testing
    anything. Excluding it by what its command references, rather than by a
    hand-kept list, keeps the exclusion current as registrations change.
    """
    # The registered path is whatever CMake was given; match it spelled either way.
    roots = tuple({str(build_dir) + os.sep, str(build_dir.resolve()) + os.sep})
    out: dict[str, str] = {}
    for test in doc.get("tests", []):
        for arg in test.get("command", []):
            if arg.startswith(roots) and not os.path.exists(arg):
                out[test["name"]] = arg
                break
    return out


def select(
    data: dict[str, Any], registered: dict[str, set[str]]
) -> tuple[list[str], list[str], list[str]]:
    """Return (selected, unregistered listed tests, labels that matched nothing)."""
    chosen: set[str] = set()
    empty_labels: list[str] = []
    for label in data.get("ctest_labels", []):
        members = {name for name, labels in registered.items() if label in labels}
        if not members:
            empty_labels.append(label)
        chosen |= members
    missing: list[str] = []
    for entry in data["tests"]:
        if entry["name"] in registered:
            chosen.add(entry["name"])
        else:
            missing.append(entry["name"])
    return sorted(chosen), missing, empty_labels


def exact_regex(names: list[str]) -> str:
    return "^(" + "|".join(re.escape(n) for n in names) + ")$"


def default_jobs() -> int:
    # A share of the host, never every core: the lane is I/O and Python bound.
    return max(1, (os.cpu_count() or 2) // 2)


def run(args: argparse.Namespace) -> int:
    build_dir = pathlib.Path(args.build_dir).resolve()
    if not (build_dir / "CTestTestfile.cmake").exists():
        print(f"drift-fast: {build_dir} is not a configured build tree", file=sys.stderr)
        return 2
    data = load_manifest(pathlib.Path(args.manifest))
    problems = manifest_problems(data)
    if problems:
        for p in problems:
            print(f"drift-fast: manifest: {p}", file=sys.stderr)
        return 2
    inventory = ctest_inventory(args.ctest, build_dir)
    selected, missing, empty_labels = select(data, parse_registrations(inventory))
    unbuilt = unbuilt_artifacts(inventory, build_dir)
    if empty_labels:
        # A label that selects nothing is a broken instrument, not a clean run.
        print(
            f"drift-fast: label(s) matched no registration: {', '.join(empty_labels)}",
            file=sys.stderr,
        )
        return 2
    for name in missing:
        print(f"drift-fast: NOT CHECKED (not registered in this configuration): {name}")
    for name in [n for n in selected if n in unbuilt]:
        print(f"drift-fast: NOT CHECKED (needs a built {unbuilt[name]}): {name}")
    selected = [n for n in selected if n not in unbuilt]
    if not selected:
        print("drift-fast: nothing left to run", file=sys.stderr)
        return 2
    print(f"drift-fast: running {len(selected)} test(s) from {build_dir}", flush=True)
    cmd = [
        args.ctest,
        "--test-dir", str(build_dir),
        "-R", exact_regex(selected),
        "--output-on-failure",
        "--timeout", str(args.timeout),
        "--repeat", REPEAT,
        "-j", str(args.jobs),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    return verdict(proc.returncode, proc.stdout, len(selected))


def verdict(returncode: int, output: str, expected: int) -> int:
    for name in SKIPPED.findall(output):
        print(f"drift-fast: NOT CHECKED (skipped): {name}")
    match = SUMMARY.search(output)
    ran = int(match.group(1)) if match else 0
    if ran != expected:
        print(
            f"drift-fast: ctest ran {ran} test(s) but {expected} were selected",
            file=sys.stderr,
        )
        return 2
    if returncode != 0:
        print("drift-fast: FAILED — the tip-plus-head tree drifts; see the tests above")
        return 1
    print(f"drift-fast: OK ({expected} test(s))")
    return 0


def precondition_problems(data: dict[str, Any], text: str, name: str) -> list[str]:
    """A step the selection needs must run before the driver, or the lane goes red on the runner.

    A selected test that passes on the gate but needs something the hosted
    checkout lacks fails every run here without a real drift, which is a false
    alarm, not a catch. The manifest names each such step and the workflow must
    invoke it ahead of ``drift_fast.py run``.
    """
    problems: list[str] = []
    # A step named only in a comment does not run.
    text = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
    run_at = text.find("tools/ci/drift_fast.py run")
    for entry in data.get("workflow_preconditions", []):
        step = entry.get("step", "") if isinstance(entry, dict) else ""
        at = text.find(step) if step else -1
        if at < 0:
            problems.append(f"{name}: never runs precondition {step}")
        elif run_at >= 0 and at > run_at:
            problems.append(f"{name}: runs precondition {step} after tools/ci/drift_fast.py run")
    return problems


def check(args: argparse.Namespace) -> int:
    data = load_manifest(pathlib.Path(args.manifest))
    problems = manifest_problems(data)
    workflow = pathlib.Path(args.workflow)
    text = workflow.read_text(encoding="utf-8") if workflow.exists() else ""
    if not text:
        problems.append(f"{workflow}: missing")
    else:
        on_block = text.split("\njobs:", 1)[0]
        for event in ("pull_request", "merge_group"):
            if not re.search(rf"^\s+{event}:", on_block, re.MULTILINE):
                problems.append(f"{workflow.name}: does not trigger on {event}")
        if "tools/ci/drift_fast.py run" not in text:
            problems.append(f"{workflow.name}: never calls tools/ci/drift_fast.py run")
        if not re.search(r"^\s+name:\s*drift-fast\s*$", text, re.MULTILINE):
            problems.append(f"{workflow.name}: no job named drift-fast")
        problems.extend(precondition_problems(data, text, workflow.name))
    for p in problems:
        print(f"drift-fast: {p}", file=sys.stderr)
    if problems:
        return 1
    print("drift-fast: manifest and workflow agree")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p_run = sub.add_parser("run", help="run the drift selection against a configured tree")
    p_run.add_argument("--build-dir", required=True)
    p_run.add_argument("--manifest", default=str(MANIFEST))
    p_run.add_argument("--ctest", default=shutil.which("ctest") or "ctest")
    p_run.add_argument("--jobs", type=int, default=default_jobs())
    p_run.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT)
    p_run.set_defaults(func=run)
    p_check = sub.add_parser("check", help="static manifest/workflow contract")
    p_check.add_argument("--manifest", default=str(MANIFEST))
    p_check.add_argument("--workflow", default=str(WORKFLOW))
    p_check.set_defaults(func=check)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
