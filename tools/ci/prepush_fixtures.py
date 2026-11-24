#!/usr/bin/env python3
"""Pre-push step: the gate-script fixture tests and source-only selftests the
pushed range can reach, under a hard wall-clock cap.

CI's `Enforce version & skill sync` runs every command of its "Gate-script
fixture tests" step and every source-only selftest on each pull request. The
pre-push hook cannot afford all of them (`test_gates.py` alone is about two
minutes), so this step runs the ones a diff can break:

* The candidates are read from the workflow step itself and from
  ``tools/ci/source_selftests.json``, so this list cannot drift from CI's.
  An aggregate suite (``test_gates.py``, which only imports the TestCase
  classes of its sibling modules) is split into one candidate per module,
  running exactly the classes the aggregate imports.
* Selection is ``select_for_changes`` from ``source_selftests.py``, the
  selector gates.sh already uses, plus the declared reads in
  ``tools/ci/prepush_triggers.json`` for paths a test builds at run time.
* Tests run in priority order (a test whose own script or declared reads the
  range touches first) on a few workers until the cap. A test that the cap
  stops or never starts is reported as NOT CHECKED with the command that runs
  it; it is never reported as a pass. A cap hit does not refuse the push.

Plain Python only: no CMake configure, no compile, no receipt. It reads the
working tree, so it judges a push of HEAD.

Exit status: 0 nothing failed (some may be NOT CHECKED), 1 a test failed,
2 the step could not run.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, Callable

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import source_selftests as lane  # noqa: E402

TRIGGERS = HERE / "prepush_triggers.json"
FIXTURE_STEP = "Gate-script fixture tests"
BUDGET_SECONDS = 60.0
MAX_WORKERS = 4
GATES_COMMAND = "tools/scripts/gates.sh origin/main"
# Order hint for a test with no measured time in the triggers file.
UNKNOWN_COST = 10.0
# Seconds kept back from the test runs for killing an overrunning test and
# printing the report, so the step as a whole ends inside the cap.
CAP_RESERVE = 2.0
# A test the selector reaches only through a name or helper it shares with the
# change (priority 1) runs when it is cheap; a longer one is left to gates.sh
# and CI, and printed as NOT CHECKED, so a broad textual match cannot spend
# the whole cap.
HEURISTIC_MAX_SECONDS = 10.0
# Changing one of these changes how every candidate is chosen or run.
OWN_FILES = (
    "tools/ci/prepush_fixtures.py",
    "tools/ci/prepush_triggers.json",
    ".github/workflows/version-skill-check.yml",
)

_AGGREGATE_IMPORT = re.compile(
    r"^from\s+(test_\w+)\s+import\s+(?:\(([^)]*)\)|([^#\n]+))", re.MULTILINE)
_UNITTEST_FAILURE = re.compile(r"^(FAIL|ERROR): (\S+) \(([^)]*)\)", re.MULTILINE)
_MARKED_FAILURE = re.compile(r"^.*(?:✗|\bFAIL(?:ED)?\b|AssertionError).*$", re.MULTILINE)
_EXCEPTION_LINE = re.compile(r"^\w+(?:Error|Exception)\b.*$", re.MULTILINE)


def load_triggers(path: pathlib.Path) -> dict[str, list[str]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1 or not isinstance(data.get("reads"), dict):
        raise ValueError(f"{path}: expected schema_version 1 and a 'reads' map")
    return {str(k): [str(g) for g in v] for k, v in data["reads"].items()}


def load_costs(path: pathlib.Path) -> dict[str, float]:
    """Measured seconds per entry name: an order hint, never a pass."""
    data = json.loads(path.read_text(encoding="utf-8"))
    return {str(k): float(v) for k, v in (data.get("seconds") or {}).items()}


def fixture_entries(repo: pathlib.Path, workflow: pathlib.Path) -> tuple[list[dict], list[str]]:
    """The fixture step's commands as entries, and the ones the hook runs itself.

    A command that takes the diff base (``text_encoding_lint.py --base``) is a
    gate the hook already runs with the push's own base; it is returned
    separately rather than run twice.
    """
    entries: list[dict[str, Any]] = []
    hook_owned: list[str] = []
    for command in lane.sync_job_commands(workflow.read_text(encoding="utf-8")):
        if command["step"] != FIXTURE_STEP:
            continue
        if not command["resolved"] or any("{" in a for a in command["args"]):
            hook_owned.append(command["key"])
            continue
        if not (repo / command["script"]).is_file():
            continue
        entries.append({
            "name": command["key"],
            "argv": ["{repo}/" + command["script"], *command["args"]],
            "cwd": "{repo}",
            "timeout": lane.WORKFLOW_TIMEOUT,
            "origin": "fixture",
        })
    return entries, hook_owned


def aggregate_members(script: pathlib.Path) -> dict[str, list[str]]:
    """``{module: [classes]}`` an aggregate suite imports, or {} for a real suite.

    An aggregate defines no tests of its own and imports TestCase classes from
    two or more sibling ``test_*`` modules; unittest runs what it imports.
    """
    try:
        text = script.read_text(encoding="utf-8")
    except OSError:
        return {}
    if re.search(r"^\s*def test_", text, re.MULTILINE):
        return {}
    members: dict[str, list[str]] = {}
    for match in _AGGREGATE_IMPORT.finditer(text):
        module = match.group(1)
        if not (script.parent / f"{module}.py").is_file():
            continue
        names = re.sub(r"#[^\n]*", "", match.group(2) or match.group(3) or "")
        for part in names.replace("\\", " ").split(","):
            name = part.strip().split()
            if name:
                members.setdefault(module, [])
                if name[0] not in members[module]:
                    members[module].append(name[0])
    return members if len(members) >= 2 else {}


def expand_aggregates(entries: list[dict], repo: pathlib.Path) -> list[dict]:
    out: list[dict[str, Any]] = []
    for entry in entries:
        sources = lane.entry_sources(entry, repo)
        members = (aggregate_members(sources[0])
                   if len(entry["argv"]) == 1 and sources and not entry.get("raw") else {})
        if not members:
            out.append(entry)
            continue
        folder = sources[0].parent
        rel_folder = folder.relative_to(repo).as_posix()
        for module, classes in members.items():
            out.append({
                "name": f"{rel_folder}/{module}.py (in {sources[0].name})",
                "argv": ["-m", "unittest", *(f"{module}.{c}" for c in classes)],
                "cwd": "{repo}/" + rel_folder,
                "timeout": entry.get("timeout"),
                "origin": entry.get("origin", "fixture"),
            })
    return out


def candidates(repo: pathlib.Path, workflow: pathlib.Path, manifest: pathlib.Path,
               triggers: dict[str, list[str]],
               costs: dict[str, float] | None = None) -> tuple[list[dict], list[str]]:
    fixtures, hook_owned = fixture_entries(repo, workflow)
    entries = expand_aggregates(fixtures, repo)
    if manifest.is_file():
        for entry in lane.load_manifest(manifest):
            entries.append({**entry, "origin": "selftest"})
    for entry in entries:
        key = source_key(entry, repo)
        if key in triggers:
            entry["reads"] = triggers[key]
        if costs and entry["name"] in costs:
            entry["seconds"] = costs[entry["name"]]
    return entries, hook_owned


def source_key(entry: dict, repo: pathlib.Path) -> str:
    sources = lane.entry_sources(entry, repo)
    if not sources:
        return ""
    try:
        return sources[0].resolve().relative_to(repo.resolve()).as_posix()
    except ValueError:
        return ""


def select(entries: list[dict], changed: list[str], repo: pathlib.Path,
           workflow: pathlib.Path) -> list[dict]:
    """The entries the range reaches, highest priority first.

    Priority 0: the range changes the test's own script or a declared read.
    Priority 1: any other reason ``select_for_changes`` gives. Within a
    priority, the cheaper test first (measured seconds in the triggers file),
    so a cap hit costs the fewest tests.
    """
    try:
        workflow_rel = workflow.resolve().relative_to(repo.resolve()).as_posix()
    except ValueError:
        workflow_rel = ""
    lane_files = tuple(lane.LANE_FILES) + OWN_FILES + ((workflow_rel,) if workflow_rel else ())
    picked = lane.select_for_changes(entries, changed, repo, lane_files)
    changed_set = set(changed)

    def priority(entry: dict) -> tuple[int, float, str]:
        own = source_key(entry, repo) in changed_set
        declared = any(lane.glob_match(g, p) for g in entry.get("reads") or () for p in changed)
        entry["priority"] = 0 if own or declared else 1
        return (entry["priority"], float(entry.get("seconds", UNKNOWN_COST)), entry["name"])

    return sorted(picked, key=priority)


def select_within(entries: list[dict], changed: list[str], repo: pathlib.Path,
                  workflow: pathlib.Path, seconds: float,
                  selector: Callable[..., list[dict]] | None = None,
                  ) -> tuple[list[dict] | None, BaseException | None]:
    """``select`` bounded by ``seconds``: (None, None) when it does not finish."""
    selector = selector or select
    box: dict[str, Any] = {}

    def work() -> None:
        try:
            box["picked"] = selector(entries, changed, repo, workflow)
        except Exception as exc:  # reported as could-not-run by the caller
            box["error"] = exc

    thread = threading.Thread(target=work, daemon=True)
    thread.start()
    thread.join(max(0.0, seconds))
    if thread.is_alive():
        return None, None
    return box.get("picked"), box.get("error")


def plan(picked: list[dict]) -> tuple[list[dict], list[tuple[dict, str]]]:
    """Split a selection into what runs and what is left NOT CHECKED up front."""
    runnable: list[dict] = []
    deferred: list[tuple[dict, str]] = []
    for entry in picked:
        if needs_whole_machine(entry):
            deferred.append((entry, "needs the machine to itself"))
        elif (entry.get("priority", 0) == 1
              and float(entry.get("seconds", 0.0)) > HEURISTIC_MAX_SECONDS):
            deferred.append((entry, f"name match only, about {float(entry['seconds']):.0f} s"))
        else:
            runnable.append(entry)
    return runnable, deferred


def repro(entry: dict, repo: pathlib.Path) -> str:
    argv = [lane.contract(lane.expand(a, repo), repo).replace("{repo}/", "")
            for a in entry["argv"]]
    if entry.get("raw"):
        return " ".join(argv)
    command = "python3 " + " ".join(argv)
    cwd = entry.get("cwd")
    if cwd and cwd != "{repo}":
        return f"(cd {cwd.replace('{repo}/', '')} && {command})"
    return command


def first_failure(output: str, returncode: int | None) -> str:
    """The first failing item in a test's output, as one short line."""
    if returncode is None:
        return "stopped by the time cap"
    match = _UNITTEST_FAILURE.search(output)
    if match:
        return f"{match.group(1)}: {match.group(2)} ({match.group(3)})"[:200]
    found = _EXCEPTION_LINE.findall(output)
    if found and "Traceback (most recent call last)" in output:
        # A traceback's own last line says what failed; the source lines
        # above it only show where.
        return found[-1].strip()[:200]
    match = _MARKED_FAILURE.search(output)
    if match and match.group(0).strip() not in ("FAILED", ""):
        return match.group(0).strip()[:200]
    if found:
        return found[-1].strip()[:200]
    lines = [ln.strip() for ln in output.splitlines() if ln.strip()]
    return (f"exit {returncode}: {lines[-1]}" if lines else f"exit {returncode}, no output")[:200]


def failure_line(entry: dict, result: dict, repo: pathlib.Path) -> str:
    return (f"✗ fixture test {entry['name']}: "
            f"{first_failure(result.get('output', ''), result.get('returncode'))}"
            f" | repro: {repro(entry, repo)}")


def not_checked_line(not_checked: list[tuple[dict, str]], budget: float) -> str:
    names = ", ".join(f"{e['name']} ({why})" for e, why in not_checked)
    return (f"[pre-push] fixture tests NOT CHECKED within the {budget:g} s cap "
            f"({len(not_checked)}): {names}. CI runs them; to run them now: {GATES_COMMAND}")


def needs_whole_machine(entry: dict) -> bool:
    return lane.is_exclusive(entry) or bool(entry.get("resource_lock"))


def _run_until(entry: dict, repo: pathlib.Path, python: str, deadline: float,
               clock: Callable[[], float]) -> dict[str, Any]:
    argv = [lane.expand(a, repo) for a in entry["argv"]]
    if not entry.get("raw"):
        argv = [python, *argv]
    env = dict(os.environ)
    for key, value in (entry.get("env") or {}).items():
        env[key] = lane.expand(value, repo)
    attempts = lane.ATTEMPTS if entry.get("origin") == "selftest" else 1
    result: dict[str, Any] = {"name": entry["name"], "returncode": None, "output": "",
                              "seconds": 0.0, "attempts": 0}
    for attempt in range(1, attempts + 1):
        remaining = min(deadline - clock(), float(entry.get("timeout") or lane.DEFAULT_TIMEOUT))
        if remaining <= 0:
            break
        result["attempts"] = attempt
        scratch = None
        if entry.get("cwd"):
            cwd = lane.expand(entry["cwd"], repo)
        else:
            scratch = tempfile.TemporaryDirectory(prefix="prepush-fixture-")
            cwd = scratch.name
        start = clock()
        try:
            proc = subprocess.Popen(
                argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", start_new_session=True)
            try:
                out, _ = proc.communicate(timeout=remaining)
                result.update(returncode=proc.returncode, output=out)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except OSError:
                    proc.kill()
                out, _ = proc.communicate()
                result.update(returncode=None, output=out or "")
        except OSError as exc:
            result.update(returncode=2, output=f"could not start: {exc}\n")
        finally:
            result["seconds"] = round(result["seconds"] + clock() - start, 2)
            if scratch is not None:
                scratch.cleanup()
        if result["returncode"] in (0, None):
            break
    return result


def run_budgeted(entries: list[dict], repo: pathlib.Path, *, python: str = sys.executable,
                 budget: float = BUDGET_SECONDS, workers: int = 2,
                 clock: Callable[[], float] = time.monotonic,
                 runner: Callable[..., dict] | None = None,
                 stream=sys.stdout) -> tuple[list[tuple[dict, dict]], list[tuple[dict, str]]]:
    """Run ``entries`` in order until ``budget`` seconds pass.

    Returns (finished, not_checked): finished pairs each entry with its result;
    not_checked pairs each entry the cap stopped or never started with why.
    """
    runner = runner or _run_until
    deadline = clock() + budget
    queue = list(entries)
    lock = threading.Lock()
    finished: list[tuple[dict, dict]] = []
    not_checked: list[tuple[dict, str]] = []

    def worker() -> None:
        while True:
            with lock:
                if not queue:
                    return
                if clock() >= deadline:
                    not_checked.extend((e, "not started before the cap") for e in queue)
                    queue.clear()
                    return
                entry = queue.pop(0)
                estimate = entry.get("seconds")
                if estimate is not None and clock() + float(estimate) > deadline:
                    # Starting it would only spend the cap on a kill; a cheaper
                    # test behind it may still fit.
                    not_checked.append((entry, f"needs about {float(estimate):.0f} s, "
                                               "more than the cap has left"))
                    continue
            result = runner(entry, repo, python, deadline, clock)
            with lock:
                if result.get("returncode") is None:
                    not_checked.append((entry, "stopped by the cap"))
                else:
                    finished.append((entry, result))
                    verdict = "pass" if result["returncode"] == 0 else "FAIL"
                    print(f"  {verdict:4s}  {entry['name']}  {result['seconds']:.1f} s",
                          file=stream, flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
        for future in [pool.submit(worker) for _ in range(max(1, workers))]:
            future.result()
    order = {e["name"]: i for i, e in enumerate(entries)}
    not_checked.sort(key=lambda pair: order.get(pair[0]["name"], 0))
    return finished, not_checked


def changed_in_range(repo: pathlib.Path, base: str, head: str) -> list[str]:
    out = subprocess.run(["git", "diff", "--name-only", f"{base}...{head}"], cwd=repo,
                         capture_output=True, text=True, encoding="utf-8", check=True).stdout
    return [line for line in out.splitlines() if line]


def _rev(repo: pathlib.Path, ref: str) -> str:
    return subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"],
                          cwd=repo, capture_output=True, text=True, encoding="utf-8").stdout.strip()


def default_jobs() -> int:
    return max(1, min(MAX_WORKERS, lane.default_workers()))


def cap_seconds(requested: float | None) -> float:
    """The cap: 60 s, or less when asked; never more."""
    value = BUDGET_SECONDS
    env = os.environ.get("PULP_PREPUSH_FIXTURE_BUDGET", "")
    for candidate in (env, requested):
        try:
            if candidate not in (None, ""):
                value = min(value, float(candidate))
        except ValueError:
            pass
    return max(0.0, value)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--base", required=True, help="diff base (the hook's BASE)")
    parser.add_argument("--head", default="HEAD", help="the pushed commit; must be HEAD")
    parser.add_argument("--repo", type=pathlib.Path, default=lane.REPO_ROOT)
    parser.add_argument("--triggers", type=pathlib.Path, default=None)
    parser.add_argument("--budget", type=float, default=None,
                        help=f"seconds, at most {BUDGET_SECONDS:g} (also PULP_PREPUSH_FIXTURE_BUDGET)")
    parser.add_argument("--jobs", type=int, default=0)
    parser.add_argument("--list", action="store_true", help="print the selection; run nothing")
    args = parser.parse_args(argv)

    started = time.monotonic()
    repo = args.repo.resolve()
    workflow = repo / ".github" / "workflows" / "version-skill-check.yml"
    manifest = repo / "tools" / "ci" / "source_selftests.json"
    triggers_path = args.triggers or (repo / "tools" / "ci" / "prepush_triggers.json")
    try:
        head = _rev(repo, args.head)
        if head and head != _rev(repo, "HEAD") and not args.list:
            print(f"[pre-push] fixture tests: NOT CHECKED, the pushed commit {head[:12]} is not "
                  f"HEAD and this step reads the working tree. Run {GATES_COMMAND} on it.")
            return 0
        changed = changed_in_range(repo, args.base, args.head)
        triggers = load_triggers(triggers_path) if triggers_path.is_file() else {}
        costs = load_costs(triggers_path) if triggers_path.is_file() else {}
        entries, hook_owned = candidates(repo, workflow, manifest, triggers, costs)
    except (OSError, ValueError, subprocess.CalledProcessError, lane.ManifestError) as exc:
        print(f"✗ fixture tests: could not select ({exc}) | repro: "
              f"python3 tools/ci/prepush_fixtures.py --base {args.base}")
        return 2

    budget = cap_seconds(args.budget)
    # Selection reads every candidate's source and the import graph; on a cold
    # cache under heavy load that alone has taken over a minute, so it runs
    # inside the cap too.
    picked, error = select_within(entries, changed, repo, workflow,
                                  budget - (time.monotonic() - started))
    if error is not None:
        print(f"✗ fixture tests: could not select ({error}) | repro: "
              f"python3 tools/ci/prepush_fixtures.py --base {args.base}")
        return 2
    if picked is None:
        print(f"[pre-push] fixture tests NOT CHECKED: choosing them did not finish within the "
              f"{budget:g} s cap (a loaded host). CI runs them; to run them now: {GATES_COMMAND}")
        return 0
    jobs = args.jobs or default_jobs()
    runnable, deferred = plan(picked)
    print(f"[pre-push] fixture tests: {len(picked)} of {len(entries)} reached by "
          f"{len(changed)} changed path(s); cap {budget:g} s, {jobs} worker(s)")
    if args.list:
        for entry in picked:
            print(f"  {entry['name']}")
        return 0

    # The cap counts from the start of this step, selection included.
    finished, not_checked = run_budgeted(
        runnable, repo, budget=max(0.0, budget - CAP_RESERVE - (time.monotonic() - started)),
        workers=jobs)
    not_checked += deferred
    failures = [(e, r) for e, r in finished if r["returncode"] != 0]
    passed = len(finished) - len(failures)
    print(f"[pre-push] fixture tests: {passed} passed, {len(failures)} failed, "
          f"{len(not_checked)} not checked in {time.monotonic() - started:.1f} s")
    for entry, result in failures:
        print(failure_line(entry, result, repo))
    if not_checked:
        print(not_checked_line(not_checked, budget))
    for entry, result in failures:
        tail = result.get("output", "").splitlines()[-40:]
        print(f"--- {entry['name']} (last {len(tail)} lines) ---")
        print("\n".join(tail))
    return 1 if failures else 0


if __name__ == "__main__":
    status = main()
    sys.stdout.flush()
    sys.stderr.flush()
    # A selection thread the cap gave up on may still be running; do not let
    # interpreter shutdown wait for it.
    os._exit(status)
