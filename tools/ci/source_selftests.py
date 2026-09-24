#!/usr/bin/env python3
"""Run source-only Python selftests on the build-free required lane.

Some ctest registrations are a Python script that reads nothing but the
checkout: a CI-tooling selftest, a source lint, a drift check. They do not
need the macOS build that the required ``macos`` gate spends ~20 minutes
producing, yet they occupied a measured ~29 % of that gate's serial
test-seconds, and six of them (``PROCESSORS 8``) ran alone in its serial tail.

``tools/ci/source_selftests.json`` names those registrations. Three things
follow from one manifest, so they cannot drift apart:

* ``test/cmake/source_selftest_lane_tests.cmake`` adds the ``source-selftest``
  label to every listed registration.
* ``tools/ci/ctest_gate_args.py`` excludes that label on the gate events
  (``pull_request``, ``workflow_dispatch``, ``merge_group``). Every other lane,
  including ``push`` to main, still runs them.
* ``.github/workflows/version-skill-check.yml`` (the required
  ``Enforce version & skill sync`` context, which reports on ``merge_group``)
  runs every entry with ``run``, so each one stays on a REQUIRED check.

``check`` is the guard. It runs on the macOS gate itself (it needs a configured
build tree) and fails when a label and the manifest disagree, when a manifest
command no longer matches its registration, when an entry reaches a build
artifact, or when either half of the wiring is gone. A test can therefore
leave the gate only by being run by the build-free lane.

Commands are stored with ``{repo}`` in place of the source root and without
the interpreter, which ``run`` supplies as ``sys.executable``.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = REPO_ROOT / "tools" / "ci" / "source_selftests.json"
LANE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "version-skill-check.yml"
LABEL = "source-selftest"
# ctest's own default when a registration sets no TIMEOUT: the gate passes
# `--timeout 120`, so the lane holds each entry to the same budget.
DEFAULT_TIMEOUT = 120.0
# The gate retries a failing test once (`--repeat until-pass:2`); the lane
# matches it so a flake that the gate tolerated cannot turn the lane red.
ATTEMPTS = 2
# A registration carrying one of these is either off the gate already or must
# keep running on the pull request head, where the source-selftest exclusion
# would silently drop it.
INCOMPATIBLE_LABELS = frozenset(
    {"pr-fast", "validation", "slow", "performance", "bench", "quality-lab"}
)
ALLOWED_PROPERTIES = frozenset(
    {"LABELS", "PROCESSORS", "RESOURCE_LOCK", "TIMEOUT", "WORKING_DIRECTORY"}
)
PYTHON_BASENAME = re.compile(r"^python(3(\.\d+)?)?(\.exe)?$")
ENTRY_KEYS = frozenset(
    {"name", "argv", "cwd", "env", "timeout", "resource_lock", "processors"}
)


class ManifestError(ValueError):
    """The manifest is malformed."""


# --------------------------------------------------------------------------
# Manifest


def load_manifest(path: pathlib.Path = MANIFEST) -> list[dict[str, Any]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise ManifestError(f"{path}: expected schema_version 1")
    tests = data.get("tests")
    if not isinstance(tests, list) or not tests:
        raise ManifestError(f"{path}: 'tests' must be a nonempty list")
    seen: set[str] = set()
    for entry in tests:
        if not isinstance(entry, dict) or set(entry) - ENTRY_KEYS:
            raise ManifestError(f"{path}: malformed entry {entry!r}")
        name = entry.get("name")
        argv = entry.get("argv")
        if not isinstance(name, str) or not name:
            raise ManifestError(f"{path}: entry without a name")
        if name in seen:
            raise ManifestError(f"{path}: duplicate entry {name}")
        seen.add(name)
        if not isinstance(argv, list) or not argv or not all(
            isinstance(a, str) for a in argv
        ):
            raise ManifestError(f"{name}: argv must be a nonempty list of strings")
    return tests


def expand(value: str, repo: pathlib.Path) -> str:
    return value.replace("{repo}", str(repo))


def contract(value: str, repo: pathlib.Path) -> str:
    return value.replace(str(repo), "{repo}")


# --------------------------------------------------------------------------
# run


class _Slots:
    """ctest's PROCESSORS: an entry occupies that many of the ``jobs`` slots.

    Several selftests fan out their own subprocesses and were calibrated to run
    alone on the gate (``PROCESSORS 8``). Scheduling them beside other work
    would measure contention rather than the code, so an entry asking for at
    least every slot runs by itself here too.
    """

    def __init__(self, total: int) -> None:
        self.total = total
        self.free = total
        self.cond = threading.Condition()

    def take(self, n: int) -> int:
        n = max(1, min(n, self.total))
        with self.cond:
            self.cond.wait_for(lambda: self.free >= n)
            self.free -= n
        return n

    def give(self, n: int) -> None:
        with self.cond:
            self.free += n
            self.cond.notify_all()


def _run_one(
    entry: dict[str, Any],
    repo: pathlib.Path,
    python: str,
    locks: dict[str, threading.Lock],
    slots: _Slots | None = None,
) -> dict[str, Any]:
    argv = [python] + [expand(a, repo) for a in entry["argv"]]
    env = {k: v for k, v in os.environ.items()}
    for key, value in (entry.get("env") or {}).items():
        env[key] = expand(value, repo)
    timeout = float(entry.get("timeout") or DEFAULT_TIMEOUT)
    held = [locks[name] for name in sorted(entry.get("resource_lock") or [])]
    result: dict[str, Any] = {"name": entry["name"], "attempts": 0}
    taken = slots.take(int(entry.get("processors") or 1)) if slots else 0
    for lock in held:
        lock.acquire()
    try:
        for attempt in range(1, ATTEMPTS + 1):
            result["attempts"] = attempt
            if entry.get("cwd"):
                cwd = expand(entry["cwd"], repo)
                scratch = None
            else:
                # ctest runs these from a directory of the build tree; the lane
                # has none, so each attempt gets an empty directory rather than
                # the checkout, which would hide an accidental cwd dependence.
                scratch = tempfile.TemporaryDirectory(prefix="source-selftest-")
                cwd = scratch.name
            start = time.monotonic()
            try:
                proc = subprocess.run(
                    argv,
                    cwd=cwd,
                    env=env,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    errors="replace",
                    timeout=timeout,
                    check=False,
                )
                result.update(returncode=proc.returncode, output=proc.stdout)
            except subprocess.TimeoutExpired as exc:
                out = exc.stdout or ""
                if isinstance(out, bytes):
                    out = out.decode("utf-8", "replace")
                result.update(returncode=None, output=out + f"\nTIMEOUT after {timeout:g}s\n")
            finally:
                result["seconds"] = round(time.monotonic() - start, 2)
                if scratch is not None:
                    scratch.cleanup()
            if result["returncode"] == 0:
                break
    finally:
        for lock in reversed(held):
            lock.release()
        if slots:
            slots.give(taken)
    return result


def run(
    entries: list[dict[str, Any]],
    *,
    repo: pathlib.Path = REPO_ROOT,
    python: str = sys.executable,
    jobs: int = 0,
    stream=sys.stdout,
) -> list[dict[str, Any]]:
    locks = {
        name: threading.Lock()
        for entry in entries
        for name in (entry.get("resource_lock") or [])
    }
    workers = jobs or max(2, (os.cpu_count() or 2))
    slots = _Slots(workers)
    # Longest first, so a heavy selftest does not start last and set the wall;
    # entries that need every slot go after the rest, as ctest defers them, so
    # a worker parked waiting for the whole machine never idles the others.
    order = sorted(
        entries,
        key=lambda e: (
            int(e.get("processors") or 1) >= workers,
            -float(e.get("timeout") or DEFAULT_TIMEOUT),
        ),
    )
    results: list[dict[str, Any]] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(_run_one, e, repo, python, locks, slots) for e in order]
        for index, future in enumerate(concurrent.futures.as_completed(futures), 1):
            res = future.result()
            results.append(res)
            verdict = "Passed" if res["returncode"] == 0 else "***Failed"
            if res["returncode"] == 0 and res["attempts"] > 1:
                verdict = "Passed (after retry)"
            print(
                f"{index:4d}/{len(entries)} {res['name']} ... {verdict} "
                f"{res['seconds']:.2f} sec",
                file=stream,
                flush=True,
            )
    return results


# --------------------------------------------------------------------------
# check


def _props(test: dict[str, Any]) -> dict[str, Any]:
    return {p["name"]: p["value"] for p in test.get("properties", [])}


def _as_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(v) for v in value]
    return [str(value)]


def check(
    entries: list[dict[str, Any]],
    inventory: dict[str, Any],
    *,
    repo: pathlib.Path,
    build_dir: pathlib.Path,
    require_all: bool,
    gate_label_excludes: dict[str, str],
    lane_workflow_text: str,
) -> list[str]:
    """Every way the two lanes could disagree, as a list of error strings."""
    errors: list[str] = []
    by_name = {e["name"]: e for e in entries}
    registered: dict[str, dict[str, Any]] = {}
    for test in inventory.get("tests", []):
        registered.setdefault(test["name"], test)

    for name, test in sorted(registered.items()):
        labels = set(_as_list(_props(test).get("LABELS")))
        if LABEL in labels and name not in by_name:
            errors.append(
                f"{name}: carries the {LABEL} label but is not in "
                f"{MANIFEST.relative_to(REPO_ROOT)}, so neither lane runs it"
            )

    build = str(build_dir.resolve())
    for name, entry in sorted(by_name.items()):
        test = registered.get(name)
        if test is None:
            if require_all:
                errors.append(f"{name}: listed in the manifest but not registered")
            continue
        props = _props(test)
        labels = set(_as_list(props.get("LABELS")))
        if LABEL not in labels:
            errors.append(f"{name}: registered without the {LABEL} label")
        bad = sorted(labels & INCOMPATIBLE_LABELS)
        if bad:
            errors.append(f"{name}: carries {bad}; it cannot move to the source lane")
        extra = sorted(set(props) - ALLOWED_PROPERTIES)
        if extra:
            errors.append(
                f"{name}: sets {extra}, which the source lane does not reproduce"
            )
        command = [str(c) for c in (test.get("command") or [])]
        if not command or not PYTHON_BASENAME.match(os.path.basename(command[0])):
            errors.append(f"{name}: registered command is not a Python interpreter")
            continue
        registered_argv = [contract(a, repo) for a in command[1:]]
        if registered_argv != entry["argv"]:
            errors.append(
                f"{name}: manifest argv {entry['argv']} does not match the "
                f"registered command {registered_argv}; rerun "
                "`tools/ci/source_selftests.py write`"
            )
        for arg in command[1:]:
            if build in arg:
                errors.append(f"{name}: argument reaches the build tree: {arg}")
            elif re.search(r"(^|=)/", contract(arg, repo)):
                errors.append(
                    f"{name}: argument names a path outside the checkout: {arg}"
                )
        cwd = str(props.get("WORKING_DIRECTORY") or "")
        want_cwd = entry.get("cwd")
        if cwd and not cwd.startswith(build):
            if contract(cwd, repo) != want_cwd:
                errors.append(f"{name}: WORKING_DIRECTORY {cwd} != manifest {want_cwd}")
        elif want_cwd:
            errors.append(f"{name}: manifest sets cwd {want_cwd}, registration does not")
        env = {}
        for item in _as_list(props.get("ENVIRONMENT")):
            key, _, value = item.partition("=")
            env[key] = contract(value, repo)
        if env != (entry.get("env") or {}):
            errors.append(f"{name}: ENVIRONMENT differs from the manifest")
        timeout = props.get("TIMEOUT")
        want_timeout = entry.get("timeout")
        if (float(timeout) if timeout else None) != (
            float(want_timeout) if want_timeout else None
        ):
            errors.append(f"{name}: TIMEOUT {timeout} != manifest {want_timeout}")
        if sorted(_as_list(props.get("RESOURCE_LOCK"))) != sorted(
            entry.get("resource_lock") or []
        ):
            errors.append(f"{name}: RESOURCE_LOCK differs from the manifest")
        if int(props.get("PROCESSORS") or 1) != int(entry.get("processors") or 1):
            errors.append(f"{name}: PROCESSORS differs from the manifest")

    for event, value in sorted(gate_label_excludes.items()):
        if LABEL not in value.split("|"):
            errors.append(
                f"gate event {event} does not exclude {LABEL}; the moved tests "
                "would run twice (harmless) but the wiring has drifted"
            )
    invocation = "tools/ci/source_selftests.py run"
    if invocation not in lane_workflow_text:
        errors.append(
            f"{LANE_WORKFLOW.relative_to(REPO_ROOT)} no longer invokes "
            f"`{invocation}`; the {LABEL} tests would run on no required lane"
        )
    return errors


def _inventory(build_dir: pathlib.Path, ctest: str) -> dict[str, Any]:
    proc = subprocess.run(
        [ctest, "--show-only=json-v1", "--test-dir", str(build_dir)],
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        raise SystemExit(f"ctest listing failed: {proc.stderr.strip()}")
    return json.loads(proc.stdout)


def _gate_label_excludes() -> dict[str, str]:
    sys.path.insert(0, str(REPO_ROOT / "tools" / "ci"))
    import ctest_gate_args  # noqa: E402

    return {
        event: ctest_gate_args.label_exclude(event, "macOS")
        for event in sorted(ctest_gate_args.GATE_EVENTS)
    }


# --------------------------------------------------------------------------
# write


def entry_from_registration(test: dict[str, Any], repo: pathlib.Path, build: str) -> dict[str, Any]:
    props = _props(test)
    entry: dict[str, Any] = {
        "name": test["name"],
        "argv": [contract(str(a), repo) for a in test["command"][1:]],
    }
    cwd = str(props.get("WORKING_DIRECTORY") or "")
    if cwd and not cwd.startswith(build):
        entry["cwd"] = contract(cwd, repo)
    env = {}
    for item in _as_list(props.get("ENVIRONMENT")):
        key, _, value = item.partition("=")
        env[key] = contract(value, repo)
    if env:
        entry["env"] = env
    if props.get("TIMEOUT"):
        entry["timeout"] = float(props["TIMEOUT"])
    if props.get("RESOURCE_LOCK"):
        entry["resource_lock"] = sorted(_as_list(props["RESOURCE_LOCK"]))
    if int(props.get("PROCESSORS") or 1) > 1:
        entry["processors"] = int(props["PROCESSORS"])
    return entry


# --------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_run = sub.add_parser("run", help="execute every manifest entry")
    p_run.add_argument("--manifest", type=pathlib.Path, default=MANIFEST)
    p_run.add_argument("--jobs", type=int, default=0)
    p_run.add_argument(
        "--min-count",
        type=int,
        default=1,
        help="fail when the manifest lists fewer entries (a shrunken lane)",
    )
    p_check = sub.add_parser("check", help="verify manifest, labels and wiring")
    p_check.add_argument("--manifest", type=pathlib.Path, default=MANIFEST)
    p_check.add_argument("--build-dir", type=pathlib.Path, required=True)
    p_check.add_argument("--ctest", default="ctest")
    p_check.add_argument("--require-all", action="store_true")
    p_write = sub.add_parser(
        "write", help="refresh manifest commands from a configured build tree"
    )
    p_write.add_argument("--manifest", type=pathlib.Path, default=MANIFEST)
    p_write.add_argument("--build-dir", type=pathlib.Path, required=True)
    p_write.add_argument("--ctest", default="ctest")
    p_write.add_argument("--add", nargs="*", default=[], help="test names to add")
    args = parser.parse_args(argv)

    if args.cmd == "run":
        entries = load_manifest(args.manifest)
        if len(entries) < args.min_count:
            print(
                f"source-selftests: manifest lists {len(entries)} entries, "
                f"fewer than --min-count {args.min_count}",
                file=sys.stderr,
            )
            return 1
        start = time.monotonic()
        results = run(entries, jobs=args.jobs)
        failed = [r for r in results if r["returncode"] != 0]
        for res in failed:
            print(f"\n===== {res['name']} (exit {res['returncode']}) =====")
            print(res.get("output", "")[-6000:])
        print(
            f"\nsource-selftests: {len(results) - len(failed)} passed, "
            f"{len(failed)} failed out of {len(results)} "
            f"in {time.monotonic() - start:.1f}s"
        )
        return 1 if failed or len(results) != len(entries) else 0

    repo = REPO_ROOT
    build_dir = args.build_dir.resolve()
    inventory = _inventory(build_dir, args.ctest)
    if args.cmd == "check":
        entries = load_manifest(args.manifest)
        errors = check(
            entries,
            inventory,
            repo=repo,
            build_dir=build_dir,
            require_all=args.require_all,
            gate_label_excludes=_gate_label_excludes(),
            lane_workflow_text=LANE_WORKFLOW.read_text(encoding="utf-8"),
        )
        for err in errors:
            print(f"source-selftests: {err}", file=sys.stderr)
        if not errors:
            print(f"source-selftests: ok ({len(entries)} entries, both lanes wired)")
        return 1 if errors else 0

    # write
    data = json.loads(args.manifest.read_text(encoding="utf-8"))
    names = [e["name"] for e in data["tests"]] + list(args.add)
    registered = {t["name"]: t for t in inventory.get("tests", [])}
    missing = [n for n in names if n not in registered]
    if missing:
        print(f"source-selftests: not registered here: {missing}", file=sys.stderr)
        return 1
    data["tests"] = [
        entry_from_registration(registered[n], repo, str(build_dir))
        for n in sorted(set(names))
    ]
    args.manifest.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    print(f"source-selftests: wrote {len(data['tests'])} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
