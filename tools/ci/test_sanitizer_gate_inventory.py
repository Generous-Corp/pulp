#!/usr/bin/env python3
"""The sanitizer lanes never run a gate-inventory check.

A gate-inventory check compares a manifest generated from the gate configure
(test/ctest_script_inputs.json, the changed-surface families, the consumption
census, the source-selftest lane) against the configured build's own ctest
inventory. A sanitizer configure registers a different set of tests by design
(GPU off, other options), so there the check reports drift that is not drift.

This keeps two halves in step:
- every test registered as `<checker> --build-dir ${CMAKE_BINARY_DIR}` with a
  bare `--check`/`check` verb carries the `gate-inventory` label, and every
  labelled test is such a check;
- every ctest run in .github/workflows/sanitizers.yml either excludes that
  label or selects tests by a pattern none of those checks matches.

A new drift check registered the same way is caught here until it is labelled.
"""

from __future__ import annotations

import re
import shlex
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github" / "workflows" / "sanitizers.yml"
LABEL = "gate-inventory"

ADD_TEST = re.compile(r"add_test\(\s*NAME\s+([\w.\-]+)\s+COMMAND\b(.*?)\)\s*$", re.S | re.M)
LABELLED = re.compile(
    r"set_property\(\s*TEST\s+([\w.\-]+)\s+APPEND\s+PROPERTY\s+LABELS\s+([^)]*)\)", re.S)


def cmake_texts(repo: Path = REPO) -> dict[str, str]:
    paths = sorted((repo / "test" / "cmake").glob("*.cmake")) + [repo / "CMakeLists.txt"]
    return {str(p.relative_to(repo)): p.read_text(encoding="utf-8") for p in paths}


def gate_inventory_checks(texts: dict[str, str]) -> set[str]:
    """Tests that check a committed manifest against the configured build."""
    found = set()
    for text in texts.values():
        for name, body in ADD_TEST.findall(text):
            tokens = body.split()
            build_dir = any(
                t == "--build-dir" and "CMAKE_BINARY_DIR" in tokens[i + 1]
                for i, t in enumerate(tokens[:-1]))
            verb = any(t.strip('"') in ("--check", "check") for t in tokens)
            if build_dir and verb:
                found.add(name)
    return found


def labelled(texts: dict[str, str]) -> set[str]:
    return {name for text in texts.values()
            for name, labels in LABELLED.findall(text) if LABEL in labels.split()}


def ctest_commands(workflow: str) -> list[str]:
    """Each `ctest --test-dir ...` invocation, continuation lines joined."""
    commands, current = [], None
    for raw in workflow.splitlines():
        line = raw.split("  #", 1)[0].strip()
        if current is None and line.startswith("ctest --test-dir"):
            current = ""
        if current is None:
            continue
        current += " " + line.rstrip("\\").strip()
        if not line.endswith("\\"):
            commands.append(current.strip())
            current = None
    return commands


def sanitizer_problems(workflow: str, members: set[str]) -> list[str]:
    problems = []
    commands = ctest_commands(workflow)
    if not commands:
        return ["no ctest invocation found in the sanitizer workflow"]
    for command in commands:
        args = shlex.split(command)
        value = {flag: args[i + 1] for i, flag in enumerate(args[:-1])
                 if flag in ("--label-exclude", "-LE", "--tests-regex", "-R", "-L")}
        exclude = value.get("--label-exclude") or value.get("-LE")
        if exclude is not None:
            if LABEL not in exclude.split("|"):
                problems.append(f"{command[:60]}...: --label-exclude lacks {LABEL}")
            continue
        pattern = value.get("--tests-regex") or value.get("-R")
        if pattern is not None:
            hits = sorted(m for m in members if re.search(pattern, m))
            if hits:
                problems.append(f"{command[:60]}...: its selection includes {hits}")
            continue
        if value.get("-L") is None:
            problems.append(f"{command[:60]}...: runs every test without excluding {LABEL}")
    return problems


class GateInventoryLabelTests(unittest.TestCase):
    def test_every_gate_inventory_check_carries_the_label(self) -> None:
        texts = cmake_texts()
        self.assertEqual(sorted(gate_inventory_checks(texts)), sorted(labelled(texts)))

    def test_control_the_scan_finds_the_known_checks(self) -> None:
        # An empty scan would make the comparison above vacuous.
        known = {"script-test-inputs-drift", "changed-surface-script-families-drift",
                 "consumption-census-drift", "source-selftest-lane-contract"}
        self.assertLessEqual(known, gate_inventory_checks(cmake_texts()))

    def test_control_an_unlabelled_check_is_reported(self) -> None:
        texts = {"x.cmake": 'add_test(NAME new-drift COMMAND ${Python3_EXECUTABLE}\n'
                            '    tool.py --check --build-dir "${CMAKE_BINARY_DIR}")\n'}
        self.assertEqual(gate_inventory_checks(texts), {"new-drift"})
        self.assertEqual(labelled(texts), set())

    def test_control_a_diff_scoped_registration_check_is_not_a_member(self) -> None:
        texts = {"x.cmake": 'add_test(NAME reg COMMAND ${Python3_EXECUTABLE}\n'
                            '    tool.py --build-dir "${CMAKE_BINARY_DIR}" --check-diff-scoped)\n'}
        self.assertEqual(gate_inventory_checks(texts), set())


class SanitizerWorkflowTests(unittest.TestCase):
    def test_every_sanitizer_ctest_run_excludes_gate_inventory_checks(self) -> None:
        members = gate_inventory_checks(cmake_texts())
        self.assertEqual(
            sanitizer_problems(WORKFLOW.read_text(encoding="utf-8"), members), [])

    def test_control_the_workflow_has_broad_runs_to_check(self) -> None:
        commands = ctest_commands(WORKFLOW.read_text(encoding="utf-8"))
        self.assertGreaterEqual(sum("--label-exclude" in c for c in commands), 3)

    def test_control_a_run_without_the_label_is_reported(self) -> None:
        workflow = ("        run: |\n"
                    "          ctest --test-dir build-x --output-on-failure \\\n"
                    "            --label-exclude 'validation|slow'\n")
        self.assertEqual(len(sanitizer_problems(workflow, {"a-drift"})), 1)

    def test_control_a_regex_selecting_a_member_is_reported(self) -> None:
        workflow = "          ctest --test-dir build-x -j1 --tests-regex 'Timer|a-drift'\n"
        self.assertEqual(len(sanitizer_problems(workflow, {"a-drift"})), 1)
        workflow = "          ctest --test-dir build-x -j1 --tests-regex 'Timer'\n"
        self.assertEqual(sanitizer_problems(workflow, {"a-drift"}), [])

    def test_control_an_unfiltered_run_is_reported(self) -> None:
        workflow = "          ctest --test-dir build-x --output-on-failure\n"
        self.assertEqual(len(sanitizer_problems(workflow, {"a-drift"})), 1)


if __name__ == "__main__":
    sys.exit(unittest.main())
