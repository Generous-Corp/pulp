#!/usr/bin/env python3
"""Run, on a pull-request head, the ctests the pull request's own diff reaches.

A pull-request head runs only the `pr-fast` tier; the full suite runs in the
merge group, where one failing test ejects every pull request batched with it.
Over a week, most merge-group `macos` failures were a pull request's OWN test,
one its diff plainly reaches, failing there instead of on its head. This step
closes that gap without bringing the full suite back to pull requests: after
the fast tier, it runs the tests `pulp affected` selects for the pull
request's base→head diff, and a failure fails the required check.

Selection is the one `pulp affected` already uses
(tools/scripts/affected_targets.py → changed_surface_inventory.project_affected,
with the Shipyard changed-surface families), plus these rules:

  * a diff that touches only docs, agent skills, workflows or planning selects
    nothing extra, and so does a selection the projection could not focus:
    its "all" fallback is never taken on a pull request head (it would cost
    about 12 minutes on a third of pull requests);
  * path families the projection cannot see: `tools/cmake/**` reaches the
    `cmake-*` fixture tests, and the wide non-native tier's manifest, classifier
    or any ctest registration under `test/` reaches `wide-non-native-selftest`;
  * `pr-fast` members already ran and are left out;
  * the extra time is capped: tests run in batches, no batch starts once the
    budget is spent, and every test that did not run is listed as skipped.

A failing script-driven test is re-run from a checkout of the base; when every
test it fails also fails there, main was already red and the failure is
reported as PRE-EXISTING ON BASE instead of failing this pull request.
Compiled tests are never exempted (their binary is this pull request's).

Output: one `::notice title=pr-head-affected-tests::` JSON line
(`pulp-pr-head-affected-tests/v1`) with the counts and minutes, then any
failures. Exit 1 when a selected test failed because of this pull request, 0
otherwise (including when nothing was selected), 2 when an input could not be
read.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "scripts"))

import affected_targets  # noqa: E402
import changed_surface_inventory as inventory  # noqa: E402

SCHEMA = "pulp-pr-head-affected-tests/v1"
DEFAULT_BUDGET_SECS = 600
PR_FAST_LABEL = "pr-fast"

# A diff made only of these selects nothing extra: none of them is an input to
# a test the projection could name, and the fast tier already reads them.
NON_TEST_PREFIXES = (".agents/", ".claude", ".codex", ".github/", "docs/", "planning")
NON_TEST_SUFFIXES = (".md",)

# Paths the build graph cannot connect to the tests that read them.
PATH_FAMILIES = (
    ("cmake-fixtures", re.compile(r"^tools/cmake/"), re.compile(r"^cmake-")),
    # The wide non-native tier's contract reads its own manifest and classifier
    # and every ctest registration (it accounts for each tree-walking scanner a
    # registration adds), so a new registration can break it with no edit to
    # the tier itself.
    ("wide-non-native",
     re.compile(r"^(tools/(ci/wide_non_native_checks\.json|scripts/(classify_changes|wide_non_native)\.py)"
                r"|test/(CMakeLists\.txt|.*\.cmake))$"),
     re.compile(r"^wide-non-native-selftest$")),
)


def diff_paths(base: str, head: str, repo: Path = REPO_ROOT) -> list[str]:
    out = subprocess.run(["git", "diff", "--name-only", "--no-renames", base, head],
                         cwd=repo, capture_output=True, text=True, check=True).stdout
    return [os.path.normpath(p) for p in out.splitlines() if p]


def is_non_test_path(path: str) -> bool:
    return path.startswith(NON_TEST_PREFIXES) or path.endswith(NON_TEST_SUFFIXES)


def family_tests(paths: list[str], names: list[str]) -> dict[str, list[str]]:
    """Family name → inventory tests it adds for these changed paths."""
    added: dict[str, list[str]] = {}
    for family, path_re, test_re in PATH_FAMILIES:
        if any(path_re.search(p) for p in paths):
            added[family] = [n for n in names if test_re.search(n)]
    return added


def inventory_tests(build: Path) -> dict[str, set[str]]:
    """Every registered test name → its labels."""
    out = subprocess.run(["ctest", "--test-dir", str(build), "-N", "--show-only=json-v1"],
                         capture_output=True, text=True, check=True).stdout
    tests = {}
    for test in json.loads(out).get("tests", []):
        labels: set[str] = set()
        for prop in test.get("properties", []):
            if prop.get("name") == "LABELS":
                labels = set(prop.get("value") or [])
        tests[test["name"]] = labels
    return tests


def project(build: Path, root: Path, paths: list[str]) -> inventory.Selection:
    """`pulp affected`'s projection of these paths onto the configured build.

    The step runs right after configuring and building the pull-request head,
    so the build system is current by construction; the staleness check the
    development loop needs (an edit since the last configure) does not apply.
    """
    families = affected_targets.policy_families(root)
    if not inventory.codemodel_reply_available(build):
        return inventory.all_selection(
            "no CMake codemodel reply in the build directory", [], 0, 0,
            inventory.DEFAULT_PROJECTION_THRESHOLD)
    model = inventory.load_codemodel_targets(build)
    changed = [p for p in paths if (root / p).is_file()]
    deleted = [p for p in paths if not (root / p).exists()]
    headers = {f for f in changed if os.path.splitext(f)[1] in inventory.HEADER_EXTENSIONS}
    deps_db = inventory.header_owners(build, model, headers)
    tests = inventory.load_projection_ctest_entries(build)
    return inventory.project_affected(model, changed, deleted, deps_db, tests,
                                      inventory.DEFAULT_PROJECTION_THRESHOLD, [], families)


def select(build: Path, paths: list[str], names: dict[str, set[str]],
           root: Path = REPO_ROOT) -> dict:
    """The extra tests for this diff, and why."""
    plan: dict = {"changed": len(paths), "mode": "none", "reason": "", "families": {},
                  "tests": []}
    if not paths:
        plan["reason"] = "the diff is empty"
        return plan
    if all(is_non_test_path(p) for p in paths):
        plan["reason"] = "docs, skills, workflows or planning only"
        return plan
    chosen: list[str] = []
    selection = project(build, root, paths)
    plan["mode"] = selection.mode
    plan["reason"] = selection.reason
    if selection.mode == "focused":
        chosen.extend(selection.tests)
    families = family_tests(paths, sorted(names))
    plan["families"] = {k: len(v) for k, v in families.items()}
    for tests in families.values():
        chosen.extend(tests)
    seen: set[str] = set()
    plan["tests"] = [t for t in chosen if t in names and PR_FAST_LABEL not in names[t]
                     and not (t in seen or seen.add(t))]
    return plan


def junit_outcomes(path: Path) -> dict[str, str]:
    """Test name → `run`, `fail` or `notrun` from a ctest JUnit file."""
    outcomes: dict[str, str] = {}
    if not path.is_file():
        return outcomes
    for case in ET.parse(path).getroot().iter("testcase"):
        status = case.get("status", "")
        failed = case.find("failure") is not None or case.find("error") is not None
        if status == "fail" or failed:
            outcomes[case.get("name", "")] = "fail"
        elif status in ("notrun", "disabled") or case.find("skipped") is not None:
            outcomes[case.get("name", "")] = "notrun"
        else:
            outcomes[case.get("name", "")] = "run"
    return outcomes


def run_budgeted(build: Path, tests: list[str], budget: float, jobs: int,
                 label_exclude: str, junit_dir: Path, chunk: int) -> dict:
    """Run ``tests`` in batches until the budget is spent."""
    start = time.monotonic()
    results = {"passed": [], "failed": [], "skipped": [], "excluded_or_absent": []}
    for index in range(0, len(tests), chunk):
        batch = tests[index:index + chunk]
        if time.monotonic() - start >= budget:
            results["skipped"].extend(batch)
            continue
        listing = junit_dir / f"batch-{index // chunk}.txt"
        listing.write_text("".join(f"{t}\n" for t in batch), encoding="utf-8")
        junit = junit_dir / f"batch-{index // chunk}.xml"
        command = ["ctest", "--test-dir", str(build), "--tests-from-file", str(listing),
                   "--output-on-failure", "-C", "Release", "--timeout", "120",
                   "--repeat", "until-pass:2", "-j", str(jobs), "--output-junit", str(junit)]
        if label_exclude:
            command += ["-LE", label_exclude]
        subprocess.run(command, check=False)
        outcomes = junit_outcomes(junit)
        for test in batch:
            state = outcomes.get(test)
            if state == "fail":
                results["failed"].append(test)
            elif state == "run":
                results["passed"].append(test)
            elif state == "notrun":
                results["skipped"].append(test)
            else:
                results["excluded_or_absent"].append(test)
    results["seconds"] = round(time.monotonic() - start, 1)
    return results


def label_base_failures(build: Path, failed: list[str], base_ref: str) -> dict[str, tuple[bool, str]]:
    """Which failures main already had: test name → (pre-existing, why).

    Only a script-driven test can be judged: its script is re-run from a
    checkout of the base, against the same build, with the source_selftests
    lane's base re-run and verdict. A compiled test's binary comes from this
    pull request's build, so running it "on the base" would compare the pull
    request with itself; it stays a failure. So does a script test that passes
    when re-run here (a flake is not evidence about the base).
    """
    sys.path.insert(0, str(REPO_ROOT / "tools" / "ci"))
    try:
        import source_selftests as lane
    finally:
        sys.path.pop(0)
    out = subprocess.run(["ctest", "--test-dir", str(build), "-N", "--show-only=json-v1"],
                         capture_output=True, text=True, check=True).stdout
    entries = {}
    for test in json.loads(out).get("tests", []):
        if test.get("name") in failed and test.get("command"):
            props = {p["name"]: p["value"] for p in test.get("properties", [])}
            entries[test["name"]] = lane._entry_from_command(
                test["name"], test["command"], props, REPO_ROOT, build)
    verdicts: dict[str, tuple[bool, str]] = {}
    scripts = []
    for name in failed:
        entry = entries.get(name)
        executable = Path(entry["argv"][0]) if entry else None
        if entry is None:
            verdicts[name] = (False, "not in the ctest inventory")
        elif executable is not None and executable.resolve().is_relative_to(build.resolve()):
            verdicts[name] = (False, "a compiled test; its binary comes from this build")
        elif not any("{repo}" in arg for arg in entry["argv"][1:]):
            verdicts[name] = (False, "its command names no checkout file to take from the base")
        else:
            scripts.append(entry)
    if scripts:
        again = lane.run(scripts, stream=io.StringIO())
        still = [r for r in again if r["returncode"] != 0]
        for r in again:
            if r["returncode"] == 0:
                verdicts[r["name"]] = (False, "passed when re-run here; a flake, not the base")
        if still:
            verdicts.update(lane.rerun_on_base(still, scripts, base_ref))
    return verdicts


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--base", default="HEAD^1",
                        help="base revision; on a pull-request merge ref, its first parent")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--budget-secs", type=float, default=DEFAULT_BUDGET_SECS)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--chunk", type=int, default=0,
                        help="tests per batch (default: 8 per job)")
    parser.add_argument("--label-exclude", default="")
    parser.add_argument("--junit-dir", type=Path)
    parser.add_argument("--dry-run", action="store_true", help="print the selection only")
    args = parser.parse_args(argv)

    build = args.build_dir.resolve()
    try:
        paths = diff_paths(args.base, args.head)
        names = inventory_tests(build)
    except (subprocess.CalledProcessError, OSError, json.JSONDecodeError) as exc:
        print(f"pr-head-affected-tests: cannot read the diff or the ctest inventory: {exc}",
              file=sys.stderr)
        return 2
    plan = select(build, paths, names)
    print(f"pr-head-affected-tests: {len(plan['tests'])} extra test(s); mode={plan['mode']} "
          f"({plan['reason']}); families={plan['families']}", flush=True)
    if args.dry_run:
        print(json.dumps(plan, indent=2))
        return 0

    results = {"passed": [], "failed": [], "skipped": [], "excluded_or_absent": [],
               "seconds": 0.0}
    if plan["tests"]:
        junit_dir = args.junit_dir or Path(tempfile.mkdtemp(prefix="pr-head-affected-"))
        junit_dir.mkdir(parents=True, exist_ok=True)
        chunk = args.chunk or max(8 * args.jobs, 16)
        results = run_budgeted(build, plan["tests"], args.budget_secs, args.jobs,
                               args.label_exclude, junit_dir, chunk)
    pre_existing: list[str] = []
    if results["failed"]:
        try:
            verdicts = label_base_failures(build, results["failed"], args.base)
        except (subprocess.CalledProcessError, OSError, json.JSONDecodeError) as exc:
            print(f"pr-head-affected-tests: could not re-run failures on the base ({exc}); "
                  "they stay failures")
            verdicts = {}
        for test in results["failed"]:
            on_base, why = verdicts.get(test, (False, "not re-run on the base"))
            if on_base:
                pre_existing.append(test)
                print(f"::warning title=pr-head-affected-tests::PRE-EXISTING ON BASE: {test} fails "
                      f"on the base too — not caused by this pull request ({why})")
            else:
                print(f"pr-head-affected-tests: {test}: {why}")
        results["failed"] = [t for t in results["failed"] if t not in pre_existing]
    summary = {
        "schema": SCHEMA, "mode": plan["mode"], "reason": plan["reason"],
        "changed_files": plan["changed"], "families": plan["families"],
        "selected": len(plan["tests"]), "passed": len(results["passed"]),
        "failed": len(results["failed"]), "pre_existing_on_base": len(pre_existing),
        "skipped_for_budget": len(results["skipped"]),
        "excluded_or_absent": len(results["excluded_or_absent"]),
        "minutes": round(results["seconds"] / 60, 2), "budget_minutes": args.budget_secs / 60,
    }
    print(f"::notice title=pr-head-affected-tests::{json.dumps(summary, sort_keys=True)}")
    if results["skipped"]:
        shown = ", ".join(results["skipped"][:40])
        more = len(results["skipped"]) - 40
        print(f"pr-head-affected-tests: SKIPPED (budget spent, not run here; the merge group "
              f"runs them): {shown}{f' and {more} more' if more > 0 else ''}")
    for test in results["failed"]:
        print(f"::error title=pr-head-affected-tests::{test} failed on this pull request head; "
              "it is a test this diff reaches, and it would fail the merge group too")
    return 1 if results["failed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
