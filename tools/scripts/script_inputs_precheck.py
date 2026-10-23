#!/usr/bin/env python3
"""Build-free check: a new script-driven ctest must have a script-inputs entry.

`script_test_inputs.py --check` (the `script-test-inputs-drift` ctest) fails a
pull request whose new Python, shell or Node ctest has no entry in
test/ctest_script_inputs.json, but it needs a configured build, so it ran only
in CI and every miss cost a round-trip. This reads the CMake files the range
adds or edits, finds `add_test(NAME <name> COMMAND ...)` calls that are new at
the head and run a script, and fails when a name has no entry.

Whether a registration is in the gate configure at all depends on options and
include chains a static read cannot evaluate (examples, GPU and scene3d probes,
platform-only tests are left out of it on purpose). So a missing entry fails
only when the test's own scope (the innermost if/elseif/else branch, or the
file's top level) already holds a test the inventory lists: that scope is
evidently configured by the gate. A test in a scope with no such neighbour is
reported as NOT CHECKED, as is a NAME built from a variable or generator
expression and the old `add_test(<name> <cmd>)` form. The configured drift check
stays the authority for those and for stale entries. A deliberate exclusion
carries `# script-inputs: not-gated <reason>` on a line of the call or the line
above it.

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
import script_test_inputs  # noqa: E402

INPUTS_LIST = "test/ctest_script_inputs.json"
EXCLUSION = re.compile(r"#\s*script-inputs:\s*not-gated\b")
# What the CMake variables a registration names expand to, for classifying the
# unexpanded command with the configured check's own rule.
EXPANSIONS = {
    "${Python3_EXECUTABLE}": "python3", "${PYTHON_EXECUTABLE}": "python3",
    "${Python_EXECUTABLE}": "python3", "${CMAKE_COMMAND}": "cmake",
    "${_PULP_NODE_FOR_TESTS}": "node", "${NODE_EXECUTABLE}": "node",
    "${BASH}": "bash", "${BASH_EXECUTABLE}": "bash",
}
REGENERATE = (
    "regenerate it from a gate-profile configure:\n"
    "    npm ci --prefix tools/import-design/jsx-runtime\n"
    "    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=ON "
    "-DPULP_BUILD_EXAMPLES=OFF\n"
    "    python3 tools/scripts/script_test_inputs.py --build-dir build --write\n"
    "    git add test/ctest_script_inputs.json\n"
    "  (on a gate host, configure on a TARTCI_ROLE=light host such as m1, or a VM)\n"
    "  If the gate configure deliberately leaves the test out (an option it turns off,\n"
    "  a platform guard), regenerating cannot add it: mark the call instead with\n"
    "    # script-inputs: not-gated <reason>\n"
    "  on the add_test line or the line above it.")


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


def script_kind(command: list[str], root: Path = Path(".")) -> str:
    """How script_test_inputs.classify, the configured check's own rule, sees a
    ctest COMMAND once its interpreter variables are expanded: "python",
    "node", "shell", "python-module" or "undeclarable"."""
    expanded = [EXPANSIONS.get(token, token) for token in command if token]
    return script_test_inputs.classify(expanded, root)[0]


def runs_script(command: list[str], root: Path = Path(".")) -> bool:
    return script_kind(command, root) != "undeclarable"


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


# Conditions every gate configure satisfies. A block guarded only by these is
# transparent: its tests share the enclosing scope's evidence.
GATE_TRUE = {"Python3_Interpreter_FOUND", "Python3_FOUND", "PULP_BUILD_TESTS", "AND"}


def _scope(call: registrations.Call, siblings: list[registrations.Call]) -> tuple[int, int]:
    """(innermost non-transparent block, branch) a call sits in; elseif/else
    start a new branch."""
    block = call.block
    anchor = call
    while (block is not None and block.header.command == "if"
           and set(block.header.args) <= GATE_TRUE
           and not any(other.command in ("elseif", "else") and other.block is block
                       for other in siblings)):
        anchor = block.header
        block = block.header.block
    branch = sum(1 for other in siblings
                 if other.command in ("elseif", "else") and other.block is block
                 and other.first < anchor.first)
    return id(block), branch


def judge(path: str, text: str, new_names: set[str], listed: set[str],
          root: Path = Path(".")) -> tuple[list[str], list[str]]:
    """(missing, advisories) for the registrations in ``text`` named in ``new_names``."""
    lines = text.splitlines()
    every = registrations.calls(text)
    registrations._blocks(every)
    listed_scopes = {_scope(call, every) for call in every
                     if call.command == "add_test" and _name(call) in listed}
    missing: list[str] = []
    advisories: list[str] = []
    for call in every:
        if call.command != "add_test" or _name(call) not in new_names:
            continue
        where = f"{path}:{call.first}"
        if any(EXCLUSION.search(line) for line in lines[max(call.first - 2, 0):call.last]):
            continue
        command = _command(call)
        if command is None:
            advisories.append(f"{where}: add_test without NAME/COMMAND; not checked here")
            continue
        kind = script_kind(command, root)
        if kind == "undeclarable":
            continue
        name = _name(call)
        if kind == "python-module" and name not in listed:
            # `python -m <module>` is declared only when the module resolves to
            # a file, which depends on the test's working directory and path.
            advisories.append(f"{where}: {name} runs `python -m`; not checked here")
        elif "${" in name or "$<" in name:
            advisories.append(f"{where}: test name {name!r} is computed; not checked here")
        elif name in listed:
            continue
        elif _scope(call, every) in listed_scopes:
            missing.append(f"{where}: {name}")
        else:
            advisories.append(f"{where}: {name} has no entry, and nothing else in its "
                              "if/else branch is in the gate inventory, so the gate "
                              "configure may not register it")
    return missing, advisories


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
        new_names = {_name(call) for call in
                     new_registrations(head_text, _show(root, merge_base, path))}
        found, noted = judge(path, head_text, new_names, listed, root)
        missing.extend(found)
        advisories.extend(noted)
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
    if advisories:
        print(f"script-inputs-precheck: ok with {len(advisories)} not checked (no new "
              "script-driven ctest in a gate-configured scope lacks an entry)")
    else:
        print("script-inputs-precheck: ok (every new script-driven ctest has an entry)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
