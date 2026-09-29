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
import contextlib
import io
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, Iterator

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
# A selftest whose source branches on the host being macOS, or probes for a
# macOS-only tool, runs less (or nothing) on the Linux lane and still exits 0.
# Such a test must stay on the macOS gate. The scan is textual and deliberately
# broad: a false positive only keeps a test where it already ran.
PLATFORM_GATE_MARKERS = re.compile(
    r"""darwin"""
    r"""|mac_ver\("""
    r"""|platform\.system\(\)"""
    r"""|shutil\.which\(\s*["'](?:codesign|lipo|xcrun|security|otool|"""
    r"""install_name_tool|plutil|auval|sw_vers|hdiutil|pkgbuild|productbuild|"""
    r"""ditto|defaults|xcodebuild|notarytool|stapler|spctl)["']""",
    re.IGNORECASE,
)
# Optional third-party imports the lane does not install; a test that skips
# without one would read as a pass.
OPTIONAL_IMPORT_MARKERS = re.compile(
    r"^\s*(?:import|from)\s+(?:yaml|numpy|PIL|skimage|scipy)\b", re.MULTILINE
)


def entry_sources(entry: dict[str, Any], repo: pathlib.Path) -> list[pathlib.Path]:
    """The script file(s) an entry executes, resolved against ``repo``."""
    argv = entry["argv"]
    if entry.get("raw"):
        # A ctest command: every argument that names a checkout file.
        files = [pathlib.Path(expand(a, repo)) for a in argv[1:] if "{repo}" in a]
        return [f for f in files if f.is_file()] or [pathlib.Path(expand(argv[0], repo))]
    if argv[:2] == ["-m", "unittest"] and entry.get("cwd"):
        return [pathlib.Path(expand(entry["cwd"], repo)) / f"{argv[2]}.py"]
    return [pathlib.Path(expand(argv[0], repo))]


def portability_errors(entry: dict[str, Any], repo: pathlib.Path) -> list[str]:
    """Reasons an entry cannot prove on Linux what it proved on the macOS gate."""
    errors = []
    for path in entry_sources(entry, repo):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            errors.append(f"{entry['name']}: cannot read {path}")
            continue
        found = PLATFORM_GATE_MARKERS.search(text)
        if found:
            line = text.count("\n", 0, found.start()) + 1
            errors.append(
                f"{entry['name']}: {path.name}:{line} is platform-gated "
                f"({found.group(0)!r}); it would skip on the Linux lane and "
                "report a pass, so it must stay on the macOS gate"
            )
        found = OPTIONAL_IMPORT_MARKERS.search(text)
        if found:
            line = text.count("\n", 0, found.start()) + 1
            errors.append(
                f"{entry['name']}: {path.name}:{line} imports "
                f"{found.group(0).strip()!r}, which the lane does not install"
            )
    return errors


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


def ctest_verdict(entry: dict[str, Any], returncode: int, output: str) -> int:
    """The exit status ctest would record, given the test's properties.

    PASS_REGULAR_EXPRESSION decides pass/fail by output alone, a
    SKIP_RETURN_CODE exit is a skip (reported as a pass, marked in the
    output), and WILL_FAIL inverts the result, all as ctest does.
    """
    skip = entry.get("skip_return_code")
    if skip is not None and returncode == int(skip):
        return 0
    patterns = entry.get("pass_regex") or []
    if patterns:
        # A CMake regex `.` also matches a newline.
        returncode = 0 if any(re.search(p, output or "", re.DOTALL) for p in patterns) else 1
    if entry.get("will_fail"):
        returncode = 0 if returncode != 0 else 1
    return returncode


def _run_one(
    entry: dict[str, Any],
    repo: pathlib.Path,
    python: str,
    locks: dict[str, threading.Lock],
    slots: _Slots | None = None,
) -> dict[str, Any]:
    argv = [expand(a, repo) for a in entry["argv"]]
    if not entry.get("raw"):
        argv = [python] + argv
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
                if not pathlib.Path(cwd).is_dir():
                    result.update(returncode=1, seconds=0.0,
                                  output=f"working directory does not exist: {cwd}\n")
                    break
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
                result.update(returncode=ctest_verdict(entry, proc.returncode, proc.stdout),
                              output=proc.stdout)
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
# diff-scoped selection (local only; the required lane always runs everything)

# Changing either of these changes how every entry runs.
LANE_FILES = ("tools/ci/source_selftests.py", "tools/ci/source_selftests.json")
# Basenames too common to say which test reads them; only their full path counts.
GENERIC_BASENAMES = frozenset({
    "SKILL.md", "CMakeLists.txt", "README.md", "__init__.py", "config.toml",
    "package.json", "manifest.json", "pyproject.toml", "setup.py",
})


_WALKS = re.compile(r"\b(?:r?glob|os\.walk|iterdir|scandir)\(")
_STRING_LITERAL = re.compile(r"""["']([A-Za-z0-9_][A-Za-z0-9_./-]*)["']""")


def changed_paths(base: str, repo: pathlib.Path = REPO_ROOT) -> list[str]:
    """Paths changed since the merge-base with ``base``, committed or not."""
    merge_base = subprocess.run(
        ["git", "merge-base", base, "HEAD"], cwd=repo,
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    diff = subprocess.run(
        ["git", "diff", "--name-only", merge_base], cwd=repo,
        capture_output=True, text=True, check=True,
    ).stdout
    return [line for line in diff.splitlines() if line]


def change_tokens(path: str) -> set[str]:
    """Strings whose presence in a test's source ties it to ``path``."""
    tokens = {path}
    name = pathlib.PurePosixPath(path)
    if name.name not in GENERIC_BASENAMES:
        tokens.add(name.name)
    if name.suffix == ".py" and name.stem != "__init__":
        # A test that imports a changed module names it only by its stem.
        tokens.add(name.stem)
    return tokens


WORKFLOW_LINT = REPO_ROOT / ".github" / "workflows" / "workflow-lint.yml"
# Workflow suites that cannot run in bounded time on a developer checkout. Each
# is reported as NOT CHECKED, never as a pass; the workflow still runs it.
WORKFLOW_LOCAL_SKIPS = {
    "tools/scripts/test_generated_version_bump_check.py": (
        "replays generators that walk full git history per file: ~105 s for its "
        "whole step on CI's shallow clone, over 600 s on a full-history checkout"
    ),
    # ctest registrations (the --ctest-python lane) share this list.
    "rack-plugin-loads": (
        "compiles the Rack pack against whatever Rack SDK this host installed, so "
        "its result is host-specific and cannot be compared with the merge-base"
    ),
}
WORKFLOW_TIMEOUT = 600.0
_WORKFLOW_PYTHON = re.compile(r"^\s+python3\s+((?:tools|scripts|test)/[\w./-]+\.py)((?:\s+[\w./-]+)*)\s*$")


def workflow_entries(
    workflow: pathlib.Path = WORKFLOW_LINT, repo: pathlib.Path = REPO_ROOT
) -> list[dict[str, Any]]:
    """Every ``python3 <repo script> [args]`` line a workflow runs, as entries.

    Read from the workflow file itself so the local list and the workflow can
    never diverge. Entries run from the checkout root, as the workflow's steps
    do, with a timeout sized for a whole contract suite rather than one test.
    """
    entries: list[dict[str, Any]] = []
    seen: set[str] = set()
    for line in workflow.read_text(encoding="utf-8").splitlines():
        match = _WORKFLOW_PYTHON.match(line)
        if not match:
            continue
        script, args = match.group(1), match.group(2).split()
        name = " ".join([script, *args])
        if name in seen or not (repo / script).is_file():
            continue
        seen.add(name)
        entries.append({
            "name": name,
            "argv": ["{repo}/" + script, *args],
            "cwd": "{repo}",
            "timeout": WORKFLOW_TIMEOUT,
        })
    return entries


class _ImportClosure:
    """Repo files each script loads, through repo helpers (cached per run)."""

    def __init__(self, repo: pathlib.Path) -> None:
        self.repo = repo
        self._cache: dict[pathlib.Path, set[pathlib.Path]] = {}
        self._modules = None
        try:
            sys.path.insert(0, str(repo / "tools" / "scripts"))
            import gate_python_imports_check as imports
            self._imports = imports
        except ImportError:
            self._imports = None
        finally:
            sys.path.pop(0)

    def of(self, script: pathlib.Path) -> set[pathlib.Path]:
        if self._imports is None:
            return set()
        script = script.resolve()
        if script not in self._cache:
            if self._modules is None:
                try:
                    self._modules = self._imports.tracked_python(self.repo)
                except (OSError, subprocess.SubprocessError):
                    self._modules = {}
            self._cache[script] = self._imports.local_import_closure(
                script, self.repo, self._modules)
        return self._cache[script]


def select_for_changes(
    entries: list[dict[str, Any]],
    changed: list[str],
    repo: pathlib.Path = REPO_ROOT,
    lane_files: tuple[str, ...] = LANE_FILES,
) -> list[dict[str, Any]]:
    """Entries a diff can plausibly break: every entry when the lane itself
    changed, otherwise each entry whose own script changed, that loads a
    changed module through its repo imports, or whose source names a changed
    file. A textual reference is a heuristic that over-selects rather than
    under-selects; the required lane still runs all.
    """
    if any(path in lane_files for path in changed):
        return list(entries)
    changed_abs = {(repo / path).resolve() for path in changed}
    tokens = set().union(*(change_tokens(path) for path in changed)) if changed else set()
    word = {t: re.compile(r"(?<![A-Za-z0-9_])" + re.escape(t) + r"(?![A-Za-z0-9_])")
            for t in tokens}
    changed_py = {p for p in changed_abs if p.suffix == ".py"}
    closure = _ImportClosure(repo) if changed_py else None
    selected = []
    for entry in entries:
        sources = entry_sources(entry, repo)
        if any(src.resolve() in changed_abs for src in sources):
            selected.append(entry)
            continue
        # A changed module reaches every script that loads it, directly or
        # through a repo helper that imports it in turn.
        if closure is not None and any(closure.of(src) & changed_py for src in sources
                                       if src.suffix == ".py"):
            selected.append(entry)
            continue
        # A checker handed directories scans whatever is in them.
        scanned = [arg.rstrip("/") + "/" for arg in entry["argv"][1:]
                   if not arg.startswith("-") and (repo / arg).is_dir()]
        if any(path.startswith(tuple(scanned)) for path in changed) if scanned else False:
            selected.append(entry)
            continue
        text = ""
        for src in sources:
            try:
                text += src.read_text(encoding="utf-8", errors="replace")
            except OSError:
                pass
        if any(pattern.search(text) for pattern in word.values()):
            selected.append(entry)
            continue
        # A script that walks a directory reads files it never names.
        if _WALKS.search(text):
            walked = [lit.rstrip("/") + "/" for lit in _STRING_LITERAL.findall(text)
                      if lit and lit not in (".", "/") and "/" not in lit[:1]
                      and (repo / lit).is_dir()]
            if walked and any(path.startswith(tuple(walked)) for path in changed):
                selected.append(entry)
    return selected


# --------------------------------------------------------------------------
# a ctest label run without ctest (local only)

PR_FAST_MANIFEST = "test/cmake/pr_fast_tests.cmake"
_CMAKE_SOURCE_VARS = ("CMAKE_SOURCE_DIR", "PROJECT_SOURCE_DIR", "PULP_ROOT_DIR")
_CMAKE_ARG = re.compile(r'"((?:[^"\\]|\\.)*)"|([^\s()"]+)')


def _pin_build(value: str, build: pathlib.Path | None) -> str:
    """Keep build-tree paths absolute, so a merge-base re-run reads the same build.

    The base checkout has no build of its own; a registration that reads the
    configured inventory must see the one the branch run saw, or every such
    failure would differ on the base for a reason the branch does not share.
    """
    if build is None:
        return value
    return value.replace(str(build), "\0BUILD\0")


def _unpin_build(value: str, build: pathlib.Path | None) -> str:
    return value.replace("\0BUILD\0", str(build)) if build is not None else value


def _to_repo(value: str, repo: pathlib.Path) -> str:
    root = str(repo)
    return value.replace(root, "{repo}") if root in value else value


def _entry_from_command(name: str, argv: list[str], props: dict[str, Any],
                        repo: pathlib.Path, build: pathlib.Path | None = None) -> dict[str, Any]:
    def portable(value: str) -> str:
        return _unpin_build(_to_repo(_pin_build(value, build), repo), build)

    entry: dict[str, Any] = {"name": name, "raw": True, "argv": [portable(a) for a in argv]}
    if props.get("WORKING_DIRECTORY"):
        entry["cwd"] = portable(str(props["WORKING_DIRECTORY"]))
    if props.get("TIMEOUT"):
        entry["timeout"] = float(props["TIMEOUT"])
    if props.get("PASS_REGULAR_EXPRESSION"):
        value = props["PASS_REGULAR_EXPRESSION"]
        entry["pass_regex"] = value if isinstance(value, list) else str(value).split(";")
    if props.get("SKIP_RETURN_CODE") not in (None, ""):
        entry["skip_return_code"] = int(props["SKIP_RETURN_CODE"])
    if str(props.get("WILL_FAIL", "")).upper() in ("1", "ON", "TRUE", "YES"):
        entry["will_fail"] = True
    if props.get("ENVIRONMENT"):
        value = props["ENVIRONMENT"]
        pairs = value if isinstance(value, list) else str(value).split(";")
        entry["env"] = dict(p.split("=", 1) for p in pairs if "=" in p)
    if props.get("RESOURCE_LOCK"):
        value = props["RESOURCE_LOCK"]
        entry["resource_lock"] = value if isinstance(value, list) else str(value).split(";")
    return entry


GATES_BUILD_DIR_ENV = "PULP_GATES_BUILD_DIR"


def _cmake_generator(build: pathlib.Path) -> str:
    try:
        cache = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    match = re.search(r"^CMAKE_GENERATOR:INTERNAL=(.*)$", cache, re.MULTILINE)
    return match.group(1).strip() if match else ""


def unusable_build_reason(build: pathlib.Path, repo: pathlib.Path = REPO_ROOT) -> str:
    """Why ``build`` cannot supply the current test registrations, or ""."""
    if not (build / "CTestTestfile.cmake").is_file():
        return "not configured for tests"
    generator = _cmake_generator(build)
    if generator != "Ninja":
        return f"generator is {generator or 'unknown'}, not Ninja"
    if build_is_stale(build, repo):
        return "configured before the test manifests last changed"
    return ""


CENSUS_FILE = pathlib.Path("docs") / "status" / "consumption-profiles.json"
CENSUS_FACTS = "consumption-census-facts.json"


def census_mismatch(build: pathlib.Path, repo: pathlib.Path = REPO_ROOT,
                    ) -> tuple[str, list[str]] | None:
    """(profile, differences) when ``build`` is not the build the census measured.

    The consumption census records, per profile, the build scope it was
    measured under (tests on, examples off). A local build with the same
    feature switches but another scope gets the census's profile key and a
    different link closure, so its drift checks fail on a pristine main. The
    census itself cannot tell those apart; the lane can, from the recorded
    scope. Returns None when the build matches, or the census does not record
    this build's profile at all (the census then reports that itself).
    """
    facts_path = build / CENSUS_FACTS
    census_path = repo / CENSUS_FILE
    if not facts_path.is_file() or not census_path.is_file():
        return None
    sys.path.insert(0, str(repo / "tools" / "scripts"))
    try:
        import consumption_census as census
        facts = json.loads(facts_path.read_text(encoding="utf-8"))
        document = json.loads(census_path.read_text(encoding="utf-8"))
        key = census.profile_key(facts)
    except Exception:  # noqa: BLE001 - an unreadable census is the census's own verdict
        return None
    finally:
        sys.path.pop(0)
    recorded = document.get("profiles", {}).get(key)
    if not isinstance(recorded, dict):
        return None
    current = facts.get("features", {})
    diffs = [f"{name}={current.get(name, 'unset')} (profile {value})"
             for name, value in sorted(recorded.get("build_scope", {}).items())
             if current.get(name) != value]
    return (key, diffs) if diffs else None


def reads_census_build(entry: dict[str, Any]) -> bool:
    """A test that compares a configured build against the consumption census."""
    argv = entry.get("argv", [])
    return "--build-dir" in argv and any("consumption_census" in a for a in argv)


def set_aside_census_tests(entries: list[dict[str, Any]], build: pathlib.Path,
                           repo: pathlib.Path = REPO_ROOT) -> dict[str, str]:
    """Remove census tests a non-profile build cannot judge; name → why."""
    mismatch = census_mismatch(build, repo)
    if not mismatch:
        return {}
    profile, diffs = mismatch
    why = f"build config ≠ census profile {profile}: {', '.join(diffs)}"
    aside = {e["name"]: why for e in entries if reads_census_build(e)}
    entries[:] = [e for e in entries if e["name"] not in aside]
    return aside


def choose_build_dir(repo: pathlib.Path = REPO_ROOT,
                     env: dict[str, str] | os._Environ[str] = os.environ,
                     ) -> tuple[pathlib.Path | None, str]:
    """The configured build whose registrations the local tier should run.

    PULP_GATES_BUILD_DIR wins when it names a configured build. Otherwise the
    most recently configured `build*` directory in the checkout that uses
    Ninja and was configured after the test manifests last changed; a stale
    Makefiles `build/` beside a current `build-gate` must not win because of
    its name. Returns (None, why) when nothing qualifies.
    """
    declared = env.get(GATES_BUILD_DIR_ENV, "").strip()
    if declared:
        path = pathlib.Path(declared).expanduser()
        path = path if path.is_absolute() else repo / path
        if (path / "CTestTestfile.cmake").is_file():
            return path.resolve(), f"from {GATES_BUILD_DIR_ENV}"
        return None, f"{GATES_BUILD_DIR_ENV}={declared} is not a configured build"
    usable, rejected = [], []
    for candidate in sorted(repo.glob("build*")):
        if not (candidate / "CMakeCache.txt").is_file():
            continue
        why = unusable_build_reason(candidate, repo)
        if why:
            rejected.append(f"{candidate.name}: {why}")
        else:
            usable.append(candidate)
    if usable:
        # Prefer a build the consumption census measured (the gate's scope), so
        # its drift checks run instead of being set aside as NOT CHECKED.
        chosen = max(usable, key=lambda d: (census_mismatch(d, repo) is None,
                                            (d / "CTestTestfile.cmake").stat().st_mtime))
        note = f"most recently configured of {len(usable)} usable"
        if census_mismatch(chosen, repo) is None and len(usable) > 1:
            note = f"matches the census profile; newest such of {len(usable)} usable"
        if rejected:
            note += "; passed over " + ", ".join(rejected)
        return chosen.resolve(), note
    return None, ", ".join(rejected) or "no build*/CMakeCache.txt in the checkout"


def build_is_stale(build: pathlib.Path, repo: pathlib.Path = REPO_ROOT) -> bool:
    """A configured build older than any test manifest registers an old tier."""
    stamp = build / "CTestTestfile.cmake"
    if not stamp.is_file():
        return False
    configured = stamp.stat().st_mtime
    manifests = [repo / "test" / "CMakeLists.txt", *(repo / "test").rglob("*.cmake")]
    return any(m.is_file() and m.stat().st_mtime > configured for m in manifests)


def ctest_label_entries_from_build(
    label: str, build_dir: pathlib.Path, repo: pathlib.Path = REPO_ROOT, ctest: str = "ctest",
) -> list[dict[str, Any]]:
    """Members of ``label`` exactly as the configured build registered them."""
    proc = subprocess.run(
        [ctest, "--test-dir", str(build_dir), "-N", "-L", f"^{label}$", "--show-only=json-v1"],
        capture_output=True, text=True, check=True,
    )
    entries = []
    for test in json.loads(proc.stdout).get("tests", []):
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        if label not in (props.get("LABELS") or []) or not test.get("command"):
            continue
        entries.append(_entry_from_command(test["name"], test["command"], props, repo, build_dir))
    return entries


def _cmake_args(text: str) -> list[str]:
    return [m.group(1) if m.group(1) is not None else m.group(2)
            for m in _CMAKE_ARG.finditer(text)]


def _cmake_calls(source: str, command: str) -> list[list[str]]:
    """Argument lists of every ``command(...)`` call, parentheses balanced."""
    calls = []
    for match in re.finditer(r"(?im)^\s*" + command + r"\s*\(", source):
        depth, index = 1, match.end()
        while index < len(source) and depth:
            char = source[index]
            if char == "#" and (index == 0 or source[index - 1] != "\\"):
                index = source.find("\n", index)
                if index < 0:
                    break
                continue
            depth += {"(": 1, ")": -1}.get(char, 0)
            index += 1
        calls.append(_cmake_args(source[match.end():index - 1]))
    return calls


def ctest_label_entries_from_source(
    label: str, repo: pathlib.Path = REPO_ROOT,
) -> tuple[list[dict[str, Any]], dict[str, str]]:
    """Members of the ``pr-fast`` tier read from the CMake manifests.

    Used when no configured build exists. A member whose command needs a
    value only a configure knows (the build directory, a generator
    expression) cannot run here and is returned as not checked, with why.
    """
    if label != "pr-fast":
        raise ValueError("only the pr-fast tier has a source-side member list")
    return static_ctest_entries(pr_fast_names(repo), repo)


def pr_fast_names(repo: pathlib.Path = REPO_ROOT) -> list[str]:
    manifest = (repo / PR_FAST_MANIFEST).read_text(encoding="utf-8")
    listed = re.search(r"set\(PULP_PR_FAST_TESTS(.*?)\n\)", manifest, re.S)
    return _cmake_args(re.sub(r"#[^\n]*", "", listed.group(1))) if listed else []


def static_ctest_entries(
    names: list[str] | None, repo: pathlib.Path = REPO_ROOT,
) -> tuple[list[dict[str, Any]], dict[str, str]]:
    """Registrations read from test/**/*.cmake; every one when ``names`` is None."""
    sources = [repo / "test" / "CMakeLists.txt", *sorted((repo / "test").rglob("*.cmake"))]
    commands: dict[str, list[str]] = {}
    props: dict[str, dict[str, Any]] = {}
    for source in sources:
        if not source.is_file():
            continue
        # test/CMakeLists.txt include()s these manifests, so the current source
        # directory is test/ while the list directory is the manifest's own.
        text = (source.read_text(encoding="utf-8", errors="replace")
                .replace("${CMAKE_CURRENT_SOURCE_DIR}", str(repo / "test"))
                .replace("${CMAKE_CURRENT_LIST_DIR}", str(source.parent)))
        _collect_tests(text, commands, props)
    substitutions = {"Python3_EXECUTABLE": sys.executable, "CMAKE_CTEST_COMMAND": "ctest",
                     "CMAKE_COMMAND": "cmake", **{v: str(repo) for v in _CMAKE_SOURCE_VARS}}
    return _resolve_members(names, commands, props, substitutions, repo)


def _collect_tests(text: str, commands: dict[str, list[str]],
                   props: dict[str, dict[str, Any]]) -> None:
    for args in _cmake_calls(text, "add_test"):
        if len(args) >= 4 and args[0] == "NAME" and args[2] == "COMMAND":
            argv, rest = [], args[3:]
            while rest and rest[0] not in ("WORKING_DIRECTORY", "CONFIGURATIONS",
                                          "COMMAND_EXPAND_LISTS"):
                argv.append(rest.pop(0))
            if rest[:1] == ["WORKING_DIRECTORY"] and len(rest) > 1:
                props.setdefault(args[1], {})["WORKING_DIRECTORY"] = rest[1]
            commands[args[1]] = argv
    for args in _cmake_calls(text, "set_tests_properties"):
        if "PROPERTIES" not in args:
            continue
        at = args.index("PROPERTIES")
        pairs = args[at + 1:]
        for test in args[:at]:
            for key, value in zip(pairs[0::2], pairs[1::2]):
                props.setdefault(test, {})[key] = value


def _resolve_members(names, commands, props, substitutions, repo):
    entries, not_checked = [], {}
    for name in (names if names is not None else sorted(commands)):
        if name not in commands:
            not_checked[name] = "no add_test registration found in test/**/*.cmake"
            continue
        argv = []
        for arg in commands[name]:
            for var, value in substitutions.items():
                arg = arg.replace("${" + var + "}", value)
            argv.append(arg)
        unresolved = sorted({m for a in argv for m in re.findall(r"\$[{<][^}>]*[}>]", a)})
        if unresolved:
            not_checked[name] = "needs a configured build (" + ", ".join(unresolved) + ")"
            continue
        member_props = {k: v for k, v in props.get(name, {}).items()}
        if "WORKING_DIRECTORY" in member_props:
            wd = member_props["WORKING_DIRECTORY"]
            for var in _CMAKE_SOURCE_VARS:
                wd = wd.replace("${" + var + "}", str(repo))
            if "${" in wd:
                member_props.pop("WORKING_DIRECTORY")
            else:
                member_props["WORKING_DIRECTORY"] = wd
        entries.append(_entry_from_command(name, argv, member_props, repo))
    return entries, not_checked


def python_ctest_entries(repo: pathlib.Path = REPO_ROOT) -> list[dict[str, Any]]:
    """Every registered ctest that hands a checkout Python script to Python.

    The pr-fast tier and the source-selftest manifest are left out: gates.sh
    runs those through their own lanes.
    """
    entries, _ = static_ctest_entries(None, repo)
    skip = set(pr_fast_names(repo))
    manifest_scripts = {
        pathlib.Path(expand(e["argv"][0], repo)).resolve()
        for e in load_manifest(repo / "tools" / "ci" / "source_selftests.json")
    }
    python = []
    for entry in entries:
        argv = entry["argv"]
        if entry["name"] in skip or len(argv) < 2 or "ython" not in pathlib.Path(argv[0]).name:
            continue
        if not (argv[1].startswith("{repo}/") and argv[1].endswith(".py")):
            continue
        if pathlib.Path(expand(argv[1], repo)).resolve() in manifest_scripts:
            continue
        python.append(entry)
    return python


# --------------------------------------------------------------------------
# failures that are already on the base (local only)

_FAILING_TEST = re.compile(
    r"^(?:FAIL|ERROR): (\S+) \(([^)]+)\)\s*$"      # unittest
    r"|^FAILED (\S+)"                               # pytest -q summary
    r"|^FAIL: (.+)$",                               # plain FAIL: <what>
    re.MULTILINE,
)


def failing_test_names(output: str) -> set[str]:
    """Names of the individual failing tests a suite's output reports."""
    names = set()
    for match in _FAILING_TEST.finditer(output or ""):
        name = match.group(2) or match.group(1) or match.group(3) or match.group(4)
        if name:
            names.add(name.strip())
    return names


_TIMINGS = re.compile(r"\d+(?:\.\d+)?\s*(?:s|sec|seconds|ms)\b")


def _normalized_failure(output: str, *roots: str) -> str:
    text = output or ""
    for root in roots:
        if root:
            text = text.replace(root, "<repo>")
    return _TIMINGS.sub("<t>", text).strip()


def base_verdict(branch: dict[str, Any], base: dict[str, Any] | None,
                 branch_root: str = "", base_root: str = "") -> tuple[bool, str]:
    """Whether a branch failure is fully explained by the base failing too.

    Pre-existing when the base run also failed and every failing test the
    branch reports also fails on the base, or, for a suite whose failures
    cannot be named, when both runs failed with the same output (checkout
    paths and timings aside). A timeout, a suite the base cannot run, or a
    differing unnamed failure is not provably pre-existing, so it keeps failing.
    """
    if base is None:
        return False, "the suite does not exist on the base"
    if base["returncode"] == 0:
        return False, "passes on the base"
    if branch["returncode"] is None or base["returncode"] is None:
        return False, "timed out, so its failures cannot be compared"
    branch_names = failing_test_names(branch.get("output", ""))
    base_names = failing_test_names(base.get("output", ""))
    if not branch_names:
        if base_names or not branch.get("output"):
            return False, "its failing tests could not be named, so they cannot be compared"
        # The base run can print the branch's root too: a build tree is shared.
        if (_normalized_failure(branch["output"], branch_root)
                == _normalized_failure(base.get("output", ""), base_root, branch_root)):
            return True, "fails with the same output on the base"
        return False, "its failing tests could not be named and its output differs on the base"
    new = sorted(branch_names - base_names)
    if new:
        return False, "new failures on this branch: " + ", ".join(new)
    return True, f"{len(branch_names)} failing test(s), all failing on the base too"


@contextlib.contextmanager
def base_checkout(ref: str, repo: pathlib.Path = REPO_ROOT) -> Iterator[pathlib.Path]:
    """A throwaway detached checkout of the merge-base with ``ref``."""
    merge_base = subprocess.run(
        ["git", "merge-base", ref, "HEAD"], cwd=repo,
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    # Beside the checkout, not in a temp directory: suites that drive
    # governed-build.sh or the root configure refuse a temporary checkout, and
    # would then fail on the base for a reason the branch does not share.
    holder = pathlib.Path(tempfile.mkdtemp(prefix=".source-selftests-base-", dir=repo.parent))
    checkout = holder / "base"
    subprocess.run(
        ["git", "worktree", "add", "--quiet", "--detach", str(checkout), merge_base],
        cwd=repo, capture_output=True, text=True, check=True,
    )
    try:
        yield checkout
    finally:
        subprocess.run(["git", "worktree", "remove", "--force", str(checkout)],
                       cwd=repo, capture_output=True, text=True, check=False)
        shutil.rmtree(holder, ignore_errors=True)


def rerun_on_base(
    failed: list[dict[str, Any]], entries: list[dict[str, Any]], ref: str,
    *, jobs: int = 0, repo: pathlib.Path = REPO_ROOT,
) -> dict[str, tuple[bool, str]]:
    """Re-run each failed entry against the merge-base and judge it."""
    by_name = {e["name"]: e for e in entries}
    verdicts: dict[str, tuple[bool, str]] = {}
    with base_checkout(ref, repo) as base:
        runnable = [by_name[r["name"]] for r in failed
                    if all(src.is_file() for src in entry_sources(by_name[r["name"]], base))]
        base_results = {r["name"]: r for r in
                        run(runnable, repo=base, jobs=jobs, stream=io.StringIO())}
    for res in failed:
        verdicts[res["name"]] = base_verdict(
            res, base_results.get(res["name"]), str(repo), str(base))
    return verdicts


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
        errors.extend(portability_errors(entry, repo))
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


def _run_and_report(args: argparse.Namespace, parser: argparse.ArgumentParser,
                    entries: list[dict[str, Any]], start: float) -> int:
    results = run(entries, jobs=args.jobs)
    failed = [r for r in results if r["returncode"] != 0]
    preexisting: list[str] = []
    if failed and args.label_base_failures:
        if not args.changed_from:
            parser.error("--label-base-failures needs --changed-from BASE")
        verdicts = rerun_on_base(failed, entries, args.changed_from, jobs=args.jobs)
        for res in failed:
            on_base, why = verdicts[res["name"]]
            if on_base:
                preexisting.append(res["name"])
                print(f"source-selftests: PRE-EXISTING ON BASE: {res['name']} fails on "
                      f"{args.changed_from} too — not caused by this branch ({why})",
                      flush=True)
            else:
                print(f"source-selftests: CAUSED BY THIS BRANCH: {res['name']} ({why})",
                      flush=True)
    caused = [r for r in failed if r["name"] not in preexisting]
    for res in caused:
        print(f"\n===== {res['name']} (exit {res['returncode']}) =====")
        print(res.get("output", "")[-6000:])
    print(
        f"\nsource-selftests: {len(results) - len(failed)} passed, "
        f"{len(caused)} failed, {len(preexisting)} pre-existing on base, "
        f"out of {len(results)} in {time.monotonic() - start:.1f}s"
    )
    return 1 if caused or len(results) != len(entries) else 0


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
    p_run.add_argument(
        "--workflow",
        type=pathlib.Path,
        help="run the repo Python scripts a workflow file invokes instead of the "
        "manifest (e.g. .github/workflows/workflow-lint.yml)",
    )
    p_run.add_argument(
        "--ctest-label",
        metavar="LABEL",
        help="run the members of a ctest label (e.g. pr-fast) instead of the "
        "manifest: from --build-dir's registrations when it is configured, "
        "otherwise from the CMake manifests. The whole tier runs; it is not "
        "diff-scoped",
    )
    p_run.add_argument("--build-dir", type=pathlib.Path)
    p_run.add_argument(
        "--ctest-python",
        action="store_true",
        help="run the registered ctests that execute a checkout Python script "
        "(outside the pr-fast tier and this manifest), scoped by --changed-from",
    )
    p_run.add_argument(
        "--label-base-failures",
        action="store_true",
        help="re-run failed entries on the merge-base with --changed-from; a "
        "failure whose failing tests all fail there too is reported as "
        "pre-existing and does not fail the run",
    )
    p_run.add_argument(
        "--changed-from",
        metavar="BASE",
        help="run only entries a diff against BASE can plausibly break (local "
        "pre-push use; the required lane runs every entry)",
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
        if args.ctest_label:
            build = args.build_dir
            if build is not None and str(build) == "auto":
                build, why = choose_build_dir()
                if build is None:
                    print(f"source-selftests: NO USABLE BUILD: {why}; members that need a "
                          "configured build are not checked", flush=True)
                else:
                    print(f"source-selftests: build dir: {build} ({why})", flush=True)
            stale = build is not None and build_is_stale(build)
            if stale:
                print(f"source-selftests: {build} was configured before the test manifests "
                      "last changed; reading the members from the manifests instead", flush=True)
            if build and not stale and (build / "CTestTestfile.cmake").is_file():
                entries = ctest_label_entries_from_build(args.ctest_label, build.resolve())
                origin = f"the configured build {build}"
                for name, why in set_aside_census_tests(entries, build).items():
                    print(f"source-selftests: NOT CHECKED locally: {name} ({why})", flush=True)
            else:
                entries, not_checked = ctest_label_entries_from_source(args.ctest_label)
                origin = "the CMake manifests"
                for name, why in sorted(not_checked.items()):
                    print(f"source-selftests: NOT CHECKED locally: {name} ({why})", flush=True)
            print(f"source-selftests: {len(entries)} {args.ctest_label} member(s) from {origin}",
                  flush=True)
            if not entries:
                print("source-selftests: the tier resolved to no runnable member; the "
                      "instrument is pointed at the wrong place", file=sys.stderr)
                return 2
            return _run_and_report(args, parser, entries, time.monotonic())
        if args.ctest_python:
            entries = python_ctest_entries()
        else:
            entries = (workflow_entries(args.workflow.resolve()) if args.workflow
                       else load_manifest(args.manifest))
        if len(entries) < args.min_count:
            print(
                f"source-selftests: manifest lists {len(entries)} entries, "
                f"fewer than --min-count {args.min_count}",
                file=sys.stderr,
            )
            return 1
        if args.workflow or args.ctest_python:
            for entry in [e for e in entries if e["name"] in WORKFLOW_LOCAL_SKIPS]:
                entries.remove(entry)
                if not args.changed_from or entry in select_for_changes(
                        [entry], changed_paths(args.changed_from)):
                    print(f"source-selftests: NOT CHECKED locally: {entry['name']} "
                          f"({WORKFLOW_LOCAL_SKIPS[entry['name']]}); CI runs it", flush=True)
        if args.changed_from:
            total = len(entries)
            lane_files = LANE_FILES
            if args.workflow:
                lane_files += (args.workflow.resolve().relative_to(REPO_ROOT).as_posix(),)
            entries = select_for_changes(
                entries, changed_paths(args.changed_from), lane_files=lane_files)
            print(
                f"source-selftests: {len(entries)} of {total} entries selected "
                f"by the diff against {args.changed_from}",
                flush=True,
            )
            if not entries:
                return 0
        return _run_and_report(args, parser, entries, time.monotonic())

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
