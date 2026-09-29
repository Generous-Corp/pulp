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
import cmake_registration_impact as registration_impact  # noqa: E402

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
    # The GPU-audio provider probes compare their compiled-in Dawn/Skia identity
    # with the pinned provider. That identity is produced at configure time by
    # CMake and the identity script and read back through target properties or
    # definitions, so no source dependency links a producer edit to the probes.
    ("gpu-audio-provider-identity",
     re.compile(r"^(core/gpu_audio/CMakeLists\.txt|tools/cmake/PulpGpuAudioProvider\w*\.cmake"
                r"|tools/scripts/gpu_audio_provider_identity\.py|tools/deps/manifest\.json"
                r"|test/cmake/verify_gpu_audio_provider\w*\.cmake)$"),
     re.compile(r"^pulp-gpu-(audio-provider-identity|host-mapped-pointer-|dawn-shared-io-provider-)")),
)


def diff_paths(base: str, head: str, repo: Path = REPO_ROOT) -> list[str]:
    out = subprocess.run(["git", "diff", "--name-only", "--no-renames", base, head],
                         cwd=repo, capture_output=True, text=True, check=True).stdout
    return [os.path.normpath(p) for p in out.splitlines() if p]


def changed_lines(base: str, head: str, paths: list[str],
                  repo: Path = REPO_ROOT) -> dict[str, tuple[set[int], list[str]]]:
    """Per path: the head-side lines each hunk touches, and the removed lines.

    A pure deletion touches the lines on either side of where it was.
    """
    if not paths:
        return {}
    out = subprocess.run(["git", "diff", "-U0", "--no-renames", base, head, "--", *paths],
                         cwd=repo, capture_output=True, text=True, check=True).stdout
    changes: dict[str, tuple[set[int], list[str]]] = {}
    current: tuple[set[int], list[str]] | None = None
    for row in out.splitlines():
        if row.startswith("+++ "):
            name = row[4:]
            current = None if name == "/dev/null" else changes.setdefault(
                os.path.normpath(name[2:] if name.startswith("b/") else name), (set(), []))
            continue
        if row.startswith("--- ") or current is None:
            continue
        hunk = _HUNK.match(row)
        if hunk:
            start, count = int(hunk.group(1)), int(hunk.group(2) or 1)
            current[0].update(range(start, start + count) if count else (start, start + 1))
        elif row.startswith("-"):
            current[1].append(row[1:])
    return changes


def is_non_test_path(path: str) -> bool:
    return path.startswith(NON_TEST_PREFIXES) or path.endswith(NON_TEST_SUFFIXES)


def source_lane_contract_tests(paths: list[str], names: list[str],
                               root: Path = REPO_ROOT) -> list[str]:
    """The source-selftest lane's own contract tests, when the diff edits a lane entry.

    The lane contract reads every entry's script (it rejects platform gates and
    third-party imports), so editing any script in tools/ci/source_selftests.json
    can break it without touching the lane itself.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    try:
        import source_selftests as lane
        entries = lane.load_manifest(root / "tools" / "ci" / "source_selftests.json")
        # entry_sources resolves both `{repo}/x.py` and `-m unittest <module>` entries.
        scripts = {src.resolve().relative_to(root.resolve()).as_posix()
                   for e in entries for src in lane.entry_sources(e, root)}
    except Exception:  # noqa: BLE001 - an unreadable manifest is the lane's own failure
        return []
    finally:
        sys.path.pop(0)
    scripts |= {"tools/ci/source_selftests.json", "tools/ci/source_selftests.py"}
    if not any(p in scripts for p in paths):
        return []
    return [n for n in names if n.startswith("source-selftest-lane-")]


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


_UNOWNED = re.compile(r"^(?:source not owned by any configured target|"
                      r"header without an owning target): (.+)$")
_HUNK = re.compile(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@")
SCRIPT_INPUTS_LIST = Path("test") / "ctest_script_inputs.json"


def script_input_tests(paths: list[str], root: Path = REPO_ROOT) -> list[str]:
    """Script-driven ctests whose declared inputs the diff touches.

    test/ctest_script_inputs.json records what each script test reads (its
    imports, fixtures, the files it parses), which the build graph cannot see.
    """
    try:
        document = json.loads((root / SCRIPT_INPUTS_LIST).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    picked = []
    for name, entry in sorted((document.get("tests") or {}).items()):
        inputs = [i.rstrip("/") for i in entry.get("inputs") or []]
        if any(p == i or p.startswith(i + "/") for p in paths for i in inputs):
            picked.append(name)
    return picked


def own_tests(build: Path, root: Path, paths: list[str]) -> list[str]:
    """Tests run by the programs whose own sources the diff edits.

    A pull request that edits a test file, or the source of a probe program a
    test runs, is the likeliest to break that test, and the projection loses it
    when the rest of the diff is too wide to focus. Only executables that
    compile an edited source count, not every library dependent.
    """
    if not inventory.codemodel_reply_available(build):
        return []
    model = inventory.load_codemodel_targets(build)
    edited = {p for p in paths if os.path.splitext(p)[1] in inventory.SOURCE_EXTENSIONS}
    owners = {t.name for t in model.targets.values()
              if t.type == "EXECUTABLE" and edited & set(t.sources)}
    artifacts = {os.path.normpath(a) for t in model.targets.values() if t.name in owners
                 for a in t.artifacts}
    entries = inventory.load_projection_ctest_entries(build) or []
    return sorted({e.name for e in entries
                   if any(os.path.normpath(c) in artifacts for c in e.command)})


def registration_tests(build: Path, paths: list[str], root: Path = REPO_ROOT,
                       changes: dict[str, tuple[set[int], list[str]]] | None = None
                       ) -> list[str]:
    """Tests whose registration the diff edits.

    Editing a registration changes a test's command, properties, build flags
    or the variables they read, and none of that reaches the test through a
    source dependency. `cmake_registration_impact` names the tests and targets
    the edited lines reach (with no `changes`, every line of an edited `test/`
    CMake file counts); a target's tests are the ctest entries that run it.
    """
    edited = [p for p in paths if p.startswith("test/")
              and (p.endswith(".cmake") or p.endswith("CMakeLists.txt"))]
    if not edited:
        return []
    names: set[str] = set()
    targets: set[str] = set()
    for rel in edited:
        try:
            text = (root / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if changes is None:
            touched, removed = set(range(1, text.count("\n") + 2)), []
        else:
            touched, removed = changes.get(rel, (set(), []))
        reached = registration_impact.impact(text, touched, removed)
        names |= reached.tests
        targets |= reached.targets
    if targets and inventory.codemodel_reply_available(build):
        model = inventory.load_codemodel_targets(build)
        artifacts = {os.path.normpath(a) for t in model.targets.values()
                     if t.name in targets for a in t.artifacts}
        for entry in inventory.load_projection_ctest_entries(build) or []:
            if any(os.path.normpath(c) in artifacts for c in entry.command):
                names.add(entry.name)
    return sorted(names)


def script_reference_tests(paths: list[str], root: Path = REPO_ROOT) -> list[str]:
    """Script-driven ctests the gates.sh lanes would select for this diff.

    The same rules: the script itself changed, it loads a changed module
    through repo imports, its source names a changed file, or it walks a
    directory the diff changed something in.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    try:
        import source_selftests as lane
        entries = lane.python_ctest_entries(root)
        # No lane-file rule: editing the lane runner is a reason to run the
        # whole lane locally, not every script test on a pull-request head.
        return sorted(e["name"] for e in lane.select_for_changes(entries, paths, root,
                                                                 lane_files=()))
    except Exception:  # noqa: BLE001 - a helper that cannot read the tree selects nothing
        return []
    finally:
        sys.path.pop(0)


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
    # The development loop builds everything when one changed source has no
    # owning target, which is right for a build. Here it would throw away every
    # other file's tests, so an unowned file is set aside and the rest are
    # projected: that can only add tests the fallback would have dropped.
    set_aside: list[str] = []
    while True:
        selection = inventory.project_affected(
            model, changed, deleted, deps_db, tests,
            inventory.DEFAULT_PROJECTION_THRESHOLD, [], families)
        unowned = _UNOWNED.match(selection.reason) if selection.mode == "all" else None
        if not unowned or unowned.group(1) not in changed or len(set_aside) >= 200:
            break
        set_aside.append(unowned.group(1))
        changed = [p for p in changed if p != unowned.group(1)]
    if set_aside and selection.mode == "focused":
        selection.reason += f" (set aside {len(set_aside)} unowned file(s))"
    return selection


def select(build: Path, paths: list[str], names: dict[str, set[str]],
           root: Path = REPO_ROOT,
           changes: dict[str, tuple[set[int], list[str]]] | None = None) -> dict:
    """The extra tests for this diff, and why."""
    plan: dict = {"changed": len(paths), "mode": "none", "reason": "", "families": {},
                  "tests": []}
    if not paths:
        plan["reason"] = "the diff is empty"
        return plan
    chosen: list[str] = []
    # Declared script inputs are exact, so they apply even where the build
    # projection does not (a docs/status file a census test parses).
    inputs = script_input_tests(paths, root)
    plan["script_inputs"] = len(inputs)
    chosen.extend(inputs)
    scripts = script_reference_tests(paths, root)
    plan["script_references"] = len(scripts)
    chosen.extend(scripts)
    registered = registration_tests(build, paths, root, changes)
    plan["registrations"] = len(registered)
    chosen.extend(registered)
    if all(is_non_test_path(p) for p in paths):
        plan["reason"] = "docs, skills, workflows or planning only"
    else:
        selection = project(build, root, paths)
        plan["mode"] = selection.mode
        plan["reason"] = selection.reason
        if selection.mode == "focused":
            chosen.extend(selection.tests)
        else:
            # Too wide to focus: still run what the diff edits directly.
            own = own_tests(build, root, paths)
            plan["own_tests"] = len(own)
            chosen.extend(own)
    families = family_tests(paths, sorted(names))
    lane_tests = source_lane_contract_tests(paths, sorted(names), root)
    if lane_tests:
        families["source-selftest-lane"] = lane_tests
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
        changes = changed_lines(args.base, args.head,
                              [p for p in paths if p.startswith("test/")])
        names = inventory_tests(build)
    except (subprocess.CalledProcessError, OSError, json.JSONDecodeError) as exc:
        print(f"pr-head-affected-tests: cannot read the diff or the ctest inventory: {exc}",
              file=sys.stderr)
        return 2
    plan = select(build, paths, names, changes=changes)
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
