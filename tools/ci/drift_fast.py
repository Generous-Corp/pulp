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
gate. The required hosted ``drift-fast`` job
(``.github/workflows/drift-fast.yml``) is its caller.

It reports, rather than hides, what it could not check: a listed test this
configuration did not register (an Apple-only registration on a Linux
configure) and a test that skipped. It fails, rather than passing vacuously,
when a label selects nothing or when ctest ran a different number of tests
than were selected.

``scope`` decides how much of the lane a diff needs. For a diff
``tools/scripts/classify_changes.py`` calls docs-only it resolves the selection
from the CMake manifests without a configure and keeps only the tests that can
read a changed file (``source_selftests.select_for_docs_only``). When every
kept test runs from source on this lane (no configured build, nothing
platform-gated, no optional import) the mode is ``source`` and ``run-source``
runs just those; anything else, including every other impact class and any
error, is ``full``: bootstrap, configure and ``run`` exactly as before.

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
    docs_only = data.get("docs_only", {})
    readers = docs_only.get("prose_not_readers", []) if isinstance(docs_only, dict) else None
    if not isinstance(readers, list) or not all(
        isinstance(x, dict) and x.get("name") and x.get("why") for x in readers
    ):
        problems.append("docs_only.prose_not_readers entries need a 'name' and a 'why'")
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


def _source_selftests():
    sys.path.insert(0, str(REPO_ROOT / "tools" / "ci"))
    try:
        import source_selftests
    finally:
        sys.path.pop(0)
    return source_selftests


def _fragment_entry(name: str, repo: pathlib.Path) -> dict[str, Any] | None:
    """A test's sources from its committed script-inputs fragment, if any."""
    path = repo / "test" / "ctest_script_inputs" / "tests" / f"{name}.json"
    try:
        record = json.loads(path.read_text(encoding="utf-8"))["record"]
    except (OSError, ValueError, KeyError, TypeError):
        return None
    sources = [record.get("entry"), *record.get("inputs", [])]
    files = ["{repo}/" + s for s in sources if isinstance(s, str) and s]
    if not files:
        return None
    return {"name": name, "raw": True, "argv": ["python3", *files]}


def is_prose(path: str) -> bool:
    """Documentation under docs/ that no manifest reads (docs/status/ is data)."""
    return path.startswith("docs/") and not path.startswith("docs/status/")


# Optional imports the source path does not install. PyYAML it does install.
_UNINSTALLED_IMPORT = re.compile(
    r"^\s*(?:import|from)\s+(?:numpy|PIL|skimage|scipy)\b", re.MULTILINE
)


def docs_only_plan(
    files: list[str] | None,
    data: dict[str, Any] | None = None,
    repo: pathlib.Path = REPO_ROOT,
) -> tuple[str, list[dict[str, Any]], str]:
    """Return (mode, entries to run in ``source`` mode, why).

    ``mode`` is ``full`` unless the diff is docs-only and every listed test
    that can read it runs from source here. Fail-closed throughout: anything
    this cannot resolve keeps the full lane.
    """
    try:
        ss = _source_selftests()
        change_class = ss._load_classifier(repo).impact_class(files)
        if change_class != "docs-only":
            return "full", [], f"impact class {change_class}"
        data = data if data is not None else load_manifest()
        labels = data.get("ctest_labels", [])
        if any(label != "pr-fast" for label in labels):
            return "full", [], f"labels {labels} have no source-side member list"
        names = sorted(set(ss.pr_fast_names(repo)) | {t["name"] for t in data["tests"]})
        entries, not_checked = ss.static_ctest_entries(names, repo)
        resolved = {e["name"] for e in entries} | set(not_checked)
        if set(names) - resolved:
            return "full", [], f"unresolved: {sorted(set(names) - resolved)}"
        exempt = set()
        if all(is_prose(f) for f in files):
            exempt = {e["name"] for e in data.get("docs_only", {}).get("prose_not_readers", [])}
        # A test that needs a configure runs only in the full lane, so if it can
        # read the change the full lane runs. Its sources come from the committed
        # script-inputs fragment; a test with none fails closed. One that needs a
        # built target is never run by this lane at all.
        needs_build = []
        for name in sorted(not_checked):
            if "$<TARGET_FILE" in not_checked[name] or name in exempt:
                continue
            pseudo = _fragment_entry(name, repo)
            if pseudo is None or ss.select_for_docs_only([pseudo], files, repo):
                needs_build.append(name)
        if needs_build:
            return "full", [], f"needs a configured build and can read the change: {needs_build}"
        selected = ss.select_for_docs_only(entries, files, repo)
        blocked = []
        for entry in selected:
            for src in ss.entry_sources(entry, repo):
                try:
                    text = src.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    blocked.append(f"{entry['name']}: cannot read {src}")
                    continue
                if _UNINSTALLED_IMPORT.search(text):
                    blocked.append(f"{entry['name']}: {src.name} needs an uninstalled import")
        if blocked:
            return "full", [], "not runnable from source here: " + "; ".join(blocked[:3])
        return "source", selected, f"docs-only: {len(selected)} of {len(names)} test(s) can read it"
    except Exception as exc:  # noqa: BLE001 - any doubt runs the full lane
        return "full", [], f"scope unavailable ({exc})"


def scope(args: argparse.Namespace) -> int:
    ss = _source_selftests()
    files = ss.diff_names(args.base)
    mode, entries, why = docs_only_plan(files)
    print(f"drift-fast: scope {mode} against {args.base}: {why}")
    for entry in entries:
        print(f"drift-fast:   runs from source: {entry['name']}")
    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as fh:
            fh.write(f"mode={mode}\n")
    return 0


def run_source(args: argparse.Namespace) -> int:
    ss = _source_selftests()
    files = ss.diff_names(args.base)
    mode, entries, why = docs_only_plan(files)
    if mode != "source":
        # The scope changed under us (or failed): never report a partial pass.
        print(f"drift-fast: run-source refused: {why}; the full lane must run",
              file=sys.stderr)
        return 2
    print(f"drift-fast: {why}", flush=True)
    if not entries:
        print("drift-fast: OK (no drift test can read this docs-only change; "
              "main pushes and the nightly run every test)")
        return 0
    results = ss.run(entries, jobs=args.jobs)
    failed = [r for r in results if r["returncode"] != 0]
    for res in failed:
        print(f"\n===== {res['name']} (exit {res['returncode']}) =====")
        print(res.get("output", "")[-6000:])
    for res in results:
        if res["returncode"] == 0 and res.get("skipped"):
            print(f"drift-fast: NOT CHECKED (skipped): {res['name']}")
    if failed or len(results) != len(entries):
        print("drift-fast: FAILED — see the tests above")
        return 1
    print(f"drift-fast: OK ({len(results)} test(s) from source)")
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


def scope_problems(text: str, name: str) -> list[str]:
    """The docs fast path may replace the full lane only on mode=source.

    The full run must be gated ``!= 'source'`` so an empty or failed scope
    output still runs it, the source run gated ``== 'source'``, and the scope
    step itself must run only on pull requests.
    """
    problems: list[str] = []
    lines = text.splitlines()
    for needle, gate in (
        ("tools/ci/drift_fast.py run --build-dir", "if: steps.scope.outputs.mode != 'source'"),
        ("tools/ci/drift_fast.py run-source", "if: steps.scope.outputs.mode == 'source'"),
        # A merge group lands every entry queued ahead of it, so its own
        # entry's diff cannot scope it (tools/scripts/resolve_classify_base.py).
        ("tools/ci/drift_fast.py scope", "if: github.event_name == 'pull_request'"),
    ):
        at = next((i for i, line in enumerate(lines)
                   if needle in line and not line.lstrip().startswith("#")), None)
        if at is None:
            problems.append(f"{name}: never calls {needle}")
            continue
        start = max(i for i in range(at + 1) if lines[i].lstrip().startswith("- "))
        if not any(line.strip() == gate for line in lines[start:at]):
            problems.append(f"{name}: the step calling {needle} is not gated `{gate}`")
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
        # drift-fast is a required status check. A required check whose run is
        # filtered out or whose job is skipped never reports, and the pull
        # request waits on "Expected" forever, so the trigger must not filter
        # by path and the job must not be conditional.
        for key in ("paths", "paths-ignore"):
            if re.search(rf"^\s+{key}:", on_block, re.MULTILINE):
                problems.append(f"{workflow.name}: filters its trigger with {key}:")
        jobs_block = text.split("\njobs:", 1)[1] if "\njobs:" in text else ""
        if re.search(r"^    if:", jobs_block, re.MULTILINE):
            problems.append(f"{workflow.name}: a job-level if: can skip the required check")
        if "tools/ci/drift_fast.py run" not in text:
            problems.append(f"{workflow.name}: never calls tools/ci/drift_fast.py run")
        if not re.search(r"^\s+name:\s*drift-fast\s*$", text, re.MULTILINE):
            problems.append(f"{workflow.name}: no job named drift-fast")
        problems.extend(precondition_problems(data, text, workflow.name))
        problems.extend(scope_problems(text, workflow.name))
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
    p_scope = sub.add_parser("scope", help="full lane, or the docs-only source subset")
    p_scope.add_argument("--base", required=True, help="exact base commit of the diff")
    p_scope.set_defaults(func=scope)
    p_src = sub.add_parser("run-source", help="run the docs-only source subset")
    p_src.add_argument("--base", required=True, help="exact base commit of the diff")
    p_src.add_argument("--jobs", type=int, default=default_jobs())
    p_src.set_defaults(func=run_source)
    p_check = sub.add_parser("check", help="static manifest/workflow contract")
    p_check.add_argument("--manifest", default=str(MANIFEST))
    p_check.add_argument("--workflow", default=str(WORKFLOW))
    p_check.set_defaults(func=check)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
