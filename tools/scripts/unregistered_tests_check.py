#!/usr/bin/env python3
"""Every tools/**/test_*.py must run somewhere.

A test file nothing invokes passes forever: its assertions never execute, and a
break-confirm run on a developer's machine is the only evidence it ever had.
This guard requires each tracked tools/**/test_*.py to be invoked by at least
one of:

  * a configured ctest registration (an `entry` in test/ctest_script_inputs.json,
    which is generated from the configured test graph);
  * the source-selftest manifest (tools/ci/source_selftests.json);
  * a workflow, or a CMake file, that names the file, or hands a directory
    containing it to a test runner (`pytest <dir>`, `unittest discover -s <dir>`);
    comments are excluded, and a directory mentioned any other way (a working
    directory, an npm prefix, a sparse checkout, a sys.path entry) runs nothing.

Files that predate the guard and are not invoked are listed in
tools/scripts/unregistered_tests_baseline.json. The baseline only shrinks: a new
unregistered test file fails, and so does a baseline entry that has since been
registered or deleted (remove it). `--write-baseline` rewrites it.

    python3 tools/scripts/unregistered_tests_check.py            # check
    python3 tools/scripts/unregistered_tests_check.py --write-baseline
"""
from __future__ import annotations

import argparse
import fnmatch
import functools
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BASELINE = "tools/scripts/unregistered_tests_baseline.json"
SCRIPT_INPUTS = "test/ctest_script_inputs.json"
SOURCE_SELFTESTS = "tools/ci/source_selftests.json"
TEST_FILE = re.compile(r"^tools/.+/test_[^/]*\.py$")
# A pytest or `unittest discover` invocation and its arguments, which may
# continue onto following lines (a shell backslash, or an indented CMake
# argument line).
RUNNER = re.compile(r"(\bpytest|\bunittest\s+discover)\b"
                    r"((?:\\\n|[^\n;&|)]|\n[ \t]+(?=[-\"'$]))*)")
ARGUMENT = re.compile(r"""[^\s"'\\]+""")
# The instrument is blind if it credits fewer files than this as invoked; the
# real count is several hundred.
MIN_COVERED = 300


def tracked(repo: Path, *paths: str) -> list[str]:
    out = subprocess.run(["git", "-C", str(repo), "ls-files", "--", *paths],
                         capture_output=True, text=True, check=True).stdout
    return out.split()


def _uncommented(text: str) -> str:
    """Workflow and CMake text without `#` comment lines or trailing comments."""
    kept = []
    for line in text.splitlines():
        stripped = line.lstrip()
        if stripped.startswith("#"):
            continue
        kept.append(re.sub(r"\s#\s.*$", "", line))
    return "\n".join(kept)


def invoking_text(repo: Path) -> str:
    sources = [path for path in tracked(repo, ".github/workflows")
               if path.endswith((".yml", ".yaml"))]
    sources += [path for path in tracked(repo, "test/cmake", "tools/cmake", "CMakeLists.txt")
                if path.endswith((".cmake", "CMakeLists.txt"))]
    text = [_uncommented((repo / path).read_text(encoding="utf-8", errors="replace"))
            for path in sources]
    text.append((repo / SOURCE_SELFTESTS).read_text(encoding="utf-8"))
    return "\n".join(text)


def registered_entries(repo: Path) -> set[str]:
    data = json.loads((repo / SCRIPT_INPUTS).read_text(encoding="utf-8"))
    return {spec.get("entry") for spec in data.get("tests", {}).values() if spec.get("entry")}


def _repo_path(argument: str) -> str:
    """`${CMAKE_SOURCE_DIR}/tools/x/` or `{repo}/tools/x` as `tools/x`."""
    start = argument.find("tools/")
    if start > 0 and argument[start - 1] != "/":
        return ""
    return argument[start:].rstrip("/") if start >= 0 else ""


@functools.lru_cache(maxsize=4)
def runner_directories(text: str) -> tuple[tuple[str, str], ...]:
    """(directory, file pattern) for each directory a test runner collects."""
    found = []
    for runner, arguments in RUNNER.findall(text):
        tokens = ARGUMENT.findall(arguments)
        if runner == "pytest":
            found += [(path, "test_*.py") for path in map(_repo_path, tokens)
                      if path and not path.endswith(".py")]
            continue
        directory, pattern = "", "test*.py"
        for flag, value in zip(tokens, tokens[1:]):
            if flag in ("-s", "--start-directory"):
                directory = _repo_path(value)
            elif flag in ("-p", "--pattern"):
                pattern = value
        if directory:
            found.append((directory, pattern))
    return tuple(found)


def invoked(path: str, entries: set[str], text: str) -> bool:
    if path in entries or path in text:
        return True
    name = path.rsplit("/", 1)[-1]
    return any(path.startswith(directory + "/") and fnmatch.fnmatch(name, pattern)
               for directory, pattern in runner_directories(text))


def uncovered(repo: Path) -> tuple[list[str], int]:
    entries = registered_entries(repo)
    text = invoking_text(repo)
    files = [path for path in tracked(repo, "tools") if TEST_FILE.match(path)]
    missing = sorted(path for path in files if not invoked(path, entries, text))
    return missing, len(files) - len(missing)


def check(repo: Path, min_covered: int = MIN_COVERED) -> list[str]:
    missing, covered = uncovered(repo)
    problems = []
    if covered < min_covered:
        problems.append(f"instrument blind: only {covered} test files credited as invoked "
                        f"(expected at least {min_covered}); the sources were not read")
    baseline = json.loads((repo / BASELINE).read_text(encoding="utf-8"))
    allowed = set(baseline["unregistered"])
    for path in sorted(set(missing) - allowed):
        problems.append(f"{path}: no ctest, source-selftest, workflow or CMake invokes it; "
                        "register it, or delete it if it is dead")
    present = set(tracked(repo, "tools"))
    for path in sorted(allowed - set(missing)):
        state = "is now invoked" if path in present else "no longer exists"
        problems.append(f"{path}: {state}; remove it from {BASELINE}")
    # A note says why a baselined file cannot run in CI; one for a file that
    # is no longer baselined describes nothing.
    for path in sorted(set(baseline.get("notes") or {}) - allowed):
        problems.append(f"{path}: has a note but is not baselined; remove the note from {BASELINE}")
    print(f"test-registration: {covered} invoked, {len(missing)} not invoked "
          f"({len(allowed)} baselined)", file=sys.stderr)
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--write-baseline", action="store_true")
    args = parser.parse_args(argv)
    repo = args.repo_root.resolve()
    if args.write_baseline:
        missing, _ = uncovered(repo)
        try:
            notes = json.loads((repo / BASELINE).read_text(encoding="utf-8")).get("notes") or {}
        except (OSError, json.JSONDecodeError):
            notes = {}
        (repo / BASELINE).write_text(json.dumps({
            "reason": "test files that predate the registration guard and that no ctest, "
                      "source-selftest, workflow or CMake invokes; register or delete each, "
                      "then remove it here",
            "legacy": "tools/local-ci/ is the legacy local CI path scheduled for removal; "
                      "its test files stay baselined until it is deleted, not registered",
            "notes": {path: note for path, note in sorted(notes.items()) if path in missing},
            "unregistered": missing,
        }, indent=2) + "\n", encoding="utf-8")
        print(f"test-registration: wrote {len(missing)} baselined files to {BASELINE}")
        return 0
    problems = check(repo)
    for problem in problems:
        print(f"test-registration: {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
