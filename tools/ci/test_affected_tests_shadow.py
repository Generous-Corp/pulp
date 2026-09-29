#!/usr/bin/env python3
"""Tests for tools/ci/affected_tests_shadow.py (shadow-mode affected-test set).

A small synthetic Ninja graph stands in for the real one:

    core/a.cpp ─┐                  test/t_a.cpp ──► t_a.o ─┐
                ├► a.o ─► liba.a ─────────────────────────┴► test/pulp-test-a
    core/a.hpp ─┘                  test/t_b.cpp ──► t_b.o ──► test/pulp-test-b
    (header dep recorded by ninja -t deps, not by build.ninja)

plus a script-driven test (`python3 tools/scripts/check.py`). What must hold:
- a source change reaches the executables that link it, through the
  recorded header deps, and only those;
- order-only (`||`) inputs never propagate;
- a CMake change, an empty diff, or a changed file no edge reads (and that is
  not a known non-input) selects every test;
- script-driven tests are selected whenever a script surface changed, and not
  otherwise;
- a failure outside the selection is counted by name; a failure inside it is
  not; an unreadable input yields exit 2 and no annotation.

Run:
    python3 tools/ci/test_affected_tests_shadow.py
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import affected_tests_shadow as ats  # noqa: E402


class Fixture:
    def __init__(self, tmp: Path) -> None:
        self.src = tmp / "src"
        self.build = tmp / "src" / "build"
        for rel in ("core/a.cpp", "core/a.hpp", "test/t_a.cpp", "test/t_b.cpp",
                    "tools/scripts/check.py", "docs/x.md", "CMakeLists.txt", "misc/data.bin"):
            p = self.src / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("x\n", encoding="utf-8")
        for rel in ("test/pulp-test-a", "test/pulp-test-b"):
            p = self.build / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text("", encoding="utf-8")
        s = str(self.src)
        self.ninja = textwrap.dedent(f"""\
            rule CXX_COMPILER
              command = cc
            rule CXX_STATIC_LIBRARY_LINKER
              command = ar
            rule CXX_EXECUTABLE_LINKER
              command = ld
            build cmake_object_order_depends_target_a: phony
            build core/a.o: CXX_COMPILER {s}/core/a.cpp || cmake_object_order_depends_target_a
            build core/liba.a: CXX_STATIC_LIBRARY_LINKER core/a.o
            build test/t_a.o: CXX_COMPILER {s}/test/t_a.cpp || cmake_object_order_depends_target_a
            build test/t_b.o: CXX_COMPILER {s}/test/t_b.cpp || core/liba.a
            build test/pulp-test-a | test/pulp-test-a.stamp: CXX_EXECUTABLE_LINKER test/t_a.o core/liba.a
            build test/pulp-test-b | test/pulp-test-b.stamp: CXX_EXECUTABLE_LINKER test/t_b.o
            """)
        self.deps = textwrap.dedent(f"""\
            core/a.o: #deps 2, deps mtime 1 (VALID)
                {s}/core/a.cpp
                {s}/core/a.hpp

            test/t_a.o: #deps 1, deps mtime 1 (VALID)
                {s}/test/t_a.cpp

            test/t_b.o: #deps 1, deps mtime 1 (VALID)
                {s}/test/t_b.cpp
            """)
        b = str(self.build)
        self.inventory = {"tests": [
            {"name": "A: one", "command": [f"{b}/test/pulp-test-a", "A: one"], "properties": []},
            {"name": "A: two", "command": [f"{b}/test/pulp-test-a", "A: two"], "properties": []},
            {"name": "B: one", "command": [f"{b}/test/pulp-test-b", "B: one"], "properties": []},
            {"name": "check-script", "command": ["/usr/bin/python3", f"{s}/tools/scripts/check.py"],
             "properties": [{"name": "WORKING_DIRECTORY", "value": s}]},
            {"name": "declared-script", "command": ["/usr/bin/python3", f"{s}/tools/scripts/declared.py"],
             "properties": []},
        ]}
        self.script_inputs = {"declared-script": ["tools/scripts/declared.py", "tools/scripts/lib/"]}

    def compute(self, changed: list[str], failed: list[str] = (), script_inputs=None) -> dict:
        return ats.compute(self.build, self.src, ats.parse_build_ninja(self.ninja),
                           ats.parse_ninja_deps(self.deps), self.inventory, changed, list(failed),
                           self.script_inputs if script_inputs is None else script_inputs)


class AffectedSetTests(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.fx = Fixture(Path(tmp.name))

    def selected(self, changed, failed=()):
        r = self.fx.compute(changed, failed)
        return r, r["selected"]

    def test_source_change_reaches_linked_executables_only(self) -> None:
        r, n = self.selected(["core/a.cpp"])
        self.assertEqual(n, 2, r)  # both A cases; B links no a.o; script untouched
        self.assertFalse(r["select_all"])
        self.assertEqual(r["reason"], "graph")

    def test_header_dependency_comes_from_ninja_deps(self) -> None:
        r, n = self.selected(["core/a.hpp"])
        self.assertEqual(n, 2, r)

    def test_order_only_inputs_do_not_propagate(self) -> None:
        # t_b.o lists core/liba.a only as an order-only input.
        r, n = self.selected(["core/a.cpp"])
        self.assertEqual(n, 2, r)
        r, n = self.selected(["test/t_b.cpp"])
        self.assertEqual(n, 1, r)

    def test_cmake_change_selects_everything(self) -> None:
        r, n = self.selected(["CMakeLists.txt"])
        self.assertEqual((n, r["select_all"], r["reason"]), (5, True, "cmake changed"))

    def test_empty_diff_selects_everything(self) -> None:
        r, n = self.selected([])
        self.assertEqual((n, r["reason"]), (5, "empty diff"))

    def test_unread_changed_file_fails_closed(self) -> None:
        r, n = self.selected(["misc/data.bin"])
        self.assertEqual(n, 5, r)
        self.assertEqual(r["unread_changed"], ["misc/data.bin"])

    def test_known_non_inputs_select_nothing(self) -> None:
        r, n = self.selected(["docs/x.md"])
        self.assertEqual(n, 0, r)

    def test_script_surface_selects_only_undeclared_script_tests(self) -> None:
        # check-script has no declared inputs: any script-surface change selects it.
        # declared-script lists its inputs: an unrelated tools/ change leaves it out.
        r, n = self.selected(["tools/scripts/other.py"])
        self.assertEqual(n, 1, r)
        self.assertEqual(r["scripts_changed"], True)
        self.assertEqual((r["script_declared"], r["script_undeclared"]), (1, 1))
        self.assertEqual((r["script_selected_declared"], r["script_selected_undeclared"]), (0, 1))

    def test_declared_script_test_is_selected_by_its_inputs(self) -> None:
        # The entry script is on the command line, so the graph hop already
        # selects it; the declared list is what reaches the inputs behind it.
        # (the undeclared check-script is selected too: any script-surface
        # change is a fail-closed hit for a test without a list.)
        r, n = self.selected(["tools/scripts/declared.py"])
        self.assertEqual(n, 2, r)
        self.assertEqual(r["script_selected_undeclared"], 1, r)
        r, n = self.selected(["tools/scripts/lib/util.py"])   # directory input
        self.assertEqual(r["script_selected_declared"], 1, r)
        r, n = self.selected([ats.SCRIPT_INPUTS_LIST])        # the list itself moved
        self.assertEqual(r["script_selected_declared"], 1, r)

    def test_without_a_list_every_script_test_keeps_the_fail_closed_rule(self) -> None:
        r = self.fx.compute(["tools/scripts/other.py"], script_inputs={})
        self.assertEqual(r["selected"], 2, r)
        self.assertEqual(r["script_undeclared"], 2)

    def test_failures_outside_the_selection_are_counted_by_name(self) -> None:
        r, _ = self.selected(["core/a.cpp"], failed=["A: one", "B: one", "check-script"])
        self.assertEqual(r["failed_outside_selection"], 2)
        self.assertEqual(r["failed_outside_names"], ["B: one", "check-script"])
        r, _ = self.selected(["core/a.cpp"], failed=["A: two"])
        self.assertEqual(r["failed_outside_selection"], 0)


class ParsersAndCliTests(unittest.TestCase):
    def test_build_ninja_parser_reads_outputs_inputs_and_drops_order_only(self) -> None:
        edges = ats.parse_build_ninja("build a.o | a.d: CXX x.cpp | pch.h || phony_dep\n")
        self.assertEqual(edges, [(["a.o"], ["x.cpp", "pch.h"], "CXX")])

    def test_junit_failures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp, "j.xml")
            p.write_text('<testsuite><testcase name="ok"/><testcase name="bad"><failure/></testcase>'
                         '<testcase name="err"><error/></testcase></testsuite>', encoding="utf-8")
            self.assertEqual(ats.junit_failures(p), ["bad", "err"])

    def test_cli_exits_2_without_an_annotation_when_an_input_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            proc = subprocess.run([sys.executable, str(HERE / "affected_tests_shadow.py"),
                                   "--build-dir", tmp, "--source-root", tmp, "--base", "HEAD~1",
                                   "--selected-json", f"{tmp}/none.json", "--junit", f"{tmp}/none.xml"],
                                  capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 2, proc.stderr)
        self.assertNotIn("::notice", proc.stdout)
        self.assertIn("no verdict", proc.stderr)

    def test_cli_annotates_a_computed_verdict(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(Path(tmp))
            (fx.build / "build.ninja").write_text(fx.ninja, encoding="utf-8")
            (fx.build / "deps.txt").write_text(fx.deps, encoding="utf-8")
            (fx.build / "selected.json").write_text(json.dumps(fx.inventory), encoding="utf-8")
            (fx.build / "ctest.junit.xml").write_text(
                '<testsuite><testcase name="B: one"><failure/></testcase></testsuite>', encoding="utf-8")
            env = {"PATH": "/usr/bin:/bin:/opt/homebrew/bin", "GIT_AUTHOR_NAME": "t",
                   "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
            git = lambda *a: subprocess.run(["git", "-C", str(fx.src), *a], check=True, env=env,
                                            capture_output=True, text=True)
            git("init", "-q"); git("add", "-A"); git("commit", "-q", "-m", "base")
            (fx.src / "core/a.cpp").write_text("y\n", encoding="utf-8")
            git("commit", "-q", "-am", "touch a")
            proc = subprocess.run([sys.executable, str(HERE / "affected_tests_shadow.py"),
                                   "--build-dir", str(fx.build), "--source-root", str(fx.src),
                                   "--base", "HEAD~1", "--head", "HEAD",
                                   "--selected-json", str(fx.build / "selected.json"),
                                   "--junit", str(fx.build / "ctest.junit.xml"),
                                   "--deps-file", str(fx.build / "deps.txt"), "--event", "merge_group"],
                                  capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        line = [ln for ln in proc.stdout.splitlines() if ln.startswith("::notice title=affected-tests-shadow::")]
        self.assertEqual(len(line), 1, proc.stdout)
        rec = json.loads(line[0].split("::", 2)[2])
        self.assertEqual((rec["schema"], rec["selected"], rec["total"]), (ats.SCHEMA, 2, 5))
        self.assertEqual(rec["failed_outside_selection"], 1)
        self.assertEqual(rec["event"], "merge_group")


if __name__ == "__main__":
    unittest.main()
