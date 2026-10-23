#!/usr/bin/env python3
"""Build-free check: a new script-driven ctest must have a script-inputs entry.

`script_test_inputs.py --check` (the `script-test-inputs-drift` ctest) fails a
pull request whose new Python, shell or Node ctest has no entry in
test/ctest_script_inputs.json, but it needs a configured build, so it ran only
in CI and every miss cost a round-trip. This reads the CMake files the range
adds or edits, finds `add_test(NAME <name> COMMAND ...)` calls that are new at
the head and run a script, and fails when a name has no entry.

It reports, and never passes, what a static read cannot decide: a NAME built
from a variable or generator expression, and the old `add_test(<name> <cmd>)`
form. The configured drift check stays the authority for those, for
registrations behind an option the gate configure turns off, and for stale
entries. A deliberate exclusion carries `# script-inputs: not-gated <reason>`
on a line of the call or the line above it.

    python3 tools/scripts/script_inputs_precheck.py --base origin/main

Exit 0 when every new script-driven test has an entry (advisories may still be
printed), 1 when one does not, 2 when the range cannot be read.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "ci"))

import cmake_registration_impact as registrations  # noqa: E402

INPUTS_LIST = "test/ctest_script_inputs.json"
EXCLUSION = re.compile(r"#\s*script-inputs:\s*not-gated\b")
SCRIPT_SUFFIXES = (".py", ".sh", ".bash", ".mjs", ".js")
INTERPRETER = re.compile(
    r"(?:^|[/{])(?:python[0-9.]*|Python3?_EXECUTABLE|PYTHON_EXECUTABLE|node|NODE_EXECUTABLE|"
    r"_PULP_NODE_FOR_TESTS|bash|sh|zsh)(?:\.exe)?}?$")
REGENERATE = (
    "regenerate it from a gate-profile configure:\n"
    "    npm ci --prefix tools/import-design/jsx-runtime\n"
    "    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=ON "
    "-DPULP_BUILD_EXAMPLES=OFF\n"
    "    python3 tools/scripts/script_test_inputs.py --build-dir build --write\n"
    "    git add test/ctest_script_inputs.json\n"
    "  (on a gate host, configure on a TARTCI_ROLE=light host such as m1, or a VM)")


def _git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True,
                          text=True, encoding="utf-8", check=True).stdout


def _show(root: Path, ref: str, path: str) -> str:
    result = subprocess.run(["git", "-C", str(root), "show", f"{ref}:{path}"],
                            capture_output=True, text=True, encoding="utf-8")
    return result.stdout if result.returncode == 0 else ""


def is_cmake_file(path: str) -> bool:
    if path.startswith(("external/", "build")):
        return False
    return path.endswith(".cmake") or os.path.basename(path) == "CMakeLists.txt"


def runs_script(command: list[str]) -> bool:
    """Whether a ctest COMMAND runs a Python, shell or Node script.

    Mirrors script_test_inputs.classify on the unexpanded CMake tokens: an
    interpreter, optionally behind `cmake -E env NAME=VALUE...`, or a script
    named directly."""
    tokens = list(command)
    if (len(tokens) > 3 and re.search(r"(?:^|[/{])(?:cmake|CMAKE_COMMAND)}?$", tokens[0])
            and tokens[1:3] == ["-E", "env"]):
        tokens = tokens[3:]
        while tokens and ("=" in tokens[0].split("/", 1)[0] or tokens[0].startswith("--unset")):
            tokens = tokens[1:]
    if not tokens:
        return False
    first = tokens[0]
    if first.endswith(SCRIPT_SUFFIXES):
        return True
    if not INTERPRETER.search(first):
        return False
    rest = [t for t in tokens[1:] if t]
    if rest and (rest[0] == "-c" or rest[0].startswith("-c")) and not rest[0].startswith("--"):
        return False  # an inline snippet has no repository entry
    return True


def new_registrations(head_text: str, base_text: str) -> list[registrations.Call]:
    """add_test calls in the head text whose name the base text does not register."""
    base_names = {_name(call) for call in registrations.calls(base_text)
                  if call.command == "add_test"}
    return [call for call in registrations.calls(head_text)
            if call.command == "add_test" and _name(call) not in base_names]


def _name(call: registrations.Call) -> str:
    args = call.args
    if len(args) >= 2 and args[0] == "NAME":
        return args[1]
    return args[0] if args else ""


def _command(call: registrations.Call) -> list[str] | None:
    args = call.args
    if not args or args[0] != "NAME" or "COMMAND" not in args:
        return None
    command = args[args.index("COMMAND") + 1:]
    for keyword in ("CONFIGURATIONS", "WORKING_DIRECTORY", "COMMAND_EXPAND_LISTS"):
        if keyword in command:
            command = command[:command.index(keyword)]
    return command


def check(root: Path, base: str, head: str = "HEAD") -> tuple[list[str], list[str]]:
    """(missing, advisories) for the range base...head."""
    changed = [path for path in _git(root, "diff", "--name-only", "--no-renames",
                                     "--diff-filter=AM", f"{base}...{head}").splitlines()
               if is_cmake_file(path)]
    if not changed:
        return [], []
    merge_base = _git(root, "merge-base", base, head).strip()
    listed = set((json.loads(_show(root, head, INPUTS_LIST) or "{}").get("tests") or {}))
    missing: list[str] = []
    advisories: list[str] = []
    for path in changed:
        head_text = _show(root, head, path)
        head_lines = head_text.splitlines()
        for call in new_registrations(head_text, _show(root, merge_base, path)):
            where = f"{path}:{call.first}"
            span = head_lines[max(call.first - 2, 0):call.last]
            if any(EXCLUSION.search(line) for line in span):
                continue
            command = _command(call)
            if command is None:
                advisories.append(f"{where}: add_test without NAME/COMMAND; not checked here")
                continue
            if not runs_script(command):
                continue
            name = _name(call)
            if "${" in name or "$<" in name:
                advisories.append(f"{where}: test name {name!r} is computed; not checked here")
                continue
            if name not in listed:
                missing.append(f"{where}: {name}")
    return missing, advisories


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument("--root", type=Path, default=Path("."))
    args = parser.parse_args(argv)
    try:
        missing, advisories = check(args.root, args.base, args.head)
    except (subprocess.CalledProcessError, json.JSONDecodeError) as error:
        detail = getattr(error, "stderr", "") or str(error)
        print(f"script-inputs-precheck: cannot read {args.base}...{args.head}: "
              f"{detail.strip()}", file=sys.stderr)
        return 2
    for line in advisories:
        print(f"script-inputs-precheck: NOT CHECKED {line} (the configured "
              "script-test-inputs-drift check still runs it)", file=sys.stderr)
    if missing:
        print("script-inputs-precheck: new script-driven ctest(s) with no entry in "
              f"{INPUTS_LIST}:", file=sys.stderr)
        for line in missing:
            print(f"  {line}", file=sys.stderr)
        print(f"  CI's script-test-inputs-drift fails on this; {REGENERATE}", file=sys.stderr)
        return 1
    print("script-inputs-precheck: ok (every new script-driven ctest has an entry)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
