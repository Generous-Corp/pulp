#!/usr/bin/env python3
"""Tests for script_inputs_precheck.py against throwaway git ranges."""

from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import script_inputs_precheck as precheck  # noqa: E402
import script_test_inputs  # noqa: E402

REPO = HERE.parent.parent

MANIFEST = "test/cmake/sample_tests.cmake"


class PrecheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.git("init", "-q", "-b", "main")
        for key, value in (("user.email", "t@e"), ("user.name", "t"),
                           ("commit.gpgsign", "false")):
            self.git("config", key, value)
        self.write(MANIFEST, 'add_test(NAME old-selftest COMMAND ${Python3_EXECUTABLE}\n'
                             '    "${CMAKE_SOURCE_DIR}/tools/scripts/test_old.py")\n')
        self.inputs({"old-selftest"})
        self.commit("seed")
        self.git("checkout", "-q", "-b", "feature")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.root), *args], capture_output=True,
                              text=True, encoding="utf-8", check=True).stdout.strip()

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def append(self, text: str) -> None:
        path = self.root / MANIFEST
        path.write_text(path.read_text(encoding="utf-8") + text, encoding="utf-8")

    def inputs(self, names: set[str]) -> None:
        self.write(precheck.INPUTS_LIST, json.dumps(
            {"tests": {name: {"entry": "x.py", "inputs": [], "kind": "python"}
                       for name in sorted(names)}}))

    def commit(self, message: str) -> None:
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)

    def run_check(self) -> tuple[list[str], list[str]]:
        return precheck.check(self.root, "main")

    def exit_code(self) -> int:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return precheck.main(["--root", str(self.root), "--base", "main"])

    def test_a_new_python_ctest_without_an_entry_fails(self) -> None:
        self.append('add_test(NAME new-selftest COMMAND ${Python3_EXECUTABLE}\n'
                    '    "${CMAKE_SOURCE_DIR}/tools/scripts/test_new.py")\n')
        self.commit("test: add new-selftest")
        missing, _ = self.run_check()
        self.assertEqual(missing, [f"{MANIFEST}:3: new-selftest"])
        self.assertEqual(self.exit_code(), 1)

    def test_control_the_same_ctest_with_an_entry_passes(self) -> None:
        self.append('add_test(NAME new-selftest COMMAND ${Python3_EXECUTABLE}\n'
                    '    "${CMAKE_SOURCE_DIR}/tools/scripts/test_new.py")\n')
        self.inputs({"old-selftest", "new-selftest"})
        self.commit("test: add new-selftest with its inputs")
        self.assertEqual(self.run_check(), ([], []))
        self.assertEqual(self.exit_code(), 0)

    def test_shell_node_and_direct_scripts_count(self) -> None:
        self.append(
            'add_test(NAME sh-test COMMAND bash "${CMAKE_SOURCE_DIR}/tools/scripts/t.sh")\n'
            'add_test(NAME node-test COMMAND ${_PULP_NODE_FOR_TESTS} "${CMAKE_SOURCE_DIR}/x.test.mjs")\n'
            'add_test(NAME direct-test COMMAND ${CMAKE_SOURCE_DIR}/tools/scripts/run.sh)\n')
        self.commit("test: three script tests")
        missing, _ = self.run_check()
        self.assertEqual(sorted(line.split(": ")[1] for line in missing),
                         ["direct-test", "node-test", "sh-test"])

    def test_the_configured_classifier_decides_what_is_a_script_test(self) -> None:
        # One rule, not a second copy: a `cmake -E env` wrapper counts exactly
        # when script_test_inputs.classify unwraps it.
        command = ["${CMAKE_COMMAND}", "-E", "env", "PYTHONUTF8=1", "python3", "b.py"]
        expected = script_test_inputs.classify(
            ["cmake", "-E", "env", "PYTHONUTF8=1", "python3", "b.py"], self.root)[0] != "undeclarable"
        self.assertEqual(precheck.runs_script(command, self.root), expected)

    def test_compiled_cmake_script_and_inline_snippets_are_not_script_tests(self) -> None:
        self.append(
            'add_test(NAME compiled COMMAND pulp-test-thing)\n'
            'add_test(NAME cmake-script COMMAND ${CMAKE_COMMAND} -P ${CMAKE_SOURCE_DIR}/x.cmake)\n'
            'add_test(NAME generator COMMAND $<TARGET_FILE:pulp-cli> --help)\n'
            'add_test(NAME inline COMMAND ${Python3_EXECUTABLE} -c "print(1)")\n')
        self.commit("test: non-script tests")
        self.assertEqual(self.run_check(), ([], []))

    def test_a_computed_name_is_reported_not_passed(self) -> None:
        self.append('add_test(NAME ${_case}-selftest COMMAND ${Python3_EXECUTABLE} t.py)\n')
        self.commit("test: computed name")
        missing, advisories = self.run_check()
        self.assertEqual(missing, [])
        self.assertEqual(len(advisories), 1)
        self.assertIn("computed", advisories[0])

    def test_the_exclusion_comment_skips_a_deliberately_ungated_test(self) -> None:
        self.append('# script-inputs: not-gated registered only with PULP_BUILD_EXAMPLES\n'
                    'add_test(NAME example-only COMMAND ${Python3_EXECUTABLE} t.py)\n')
        self.commit("test: example-only")
        self.assertEqual(self.run_check(), ([], []))

    def test_editing_an_existing_registration_is_not_a_new_test(self) -> None:
        self.write(MANIFEST, 'add_test(NAME old-selftest COMMAND ${Python3_EXECUTABLE}\n'
                             '    "${CMAKE_SOURCE_DIR}/tools/scripts/test_old.py" --verbose)\n')
        self.commit("test: pass --verbose")
        self.assertEqual(self.run_check(), ([], []))

    def test_a_non_cmake_change_reads_nothing(self) -> None:
        self.write("tools/scripts/test_old.py", "print(1)\n")
        self.commit("edit the script")
        self.assertEqual(self.run_check(), ([], []))

    def test_a_test_in_a_branch_the_gate_may_not_configure_is_not_checked(self) -> None:
        self.append('if(PULP_BUILD_EXAMPLES)\n'
                    '  add_test(NAME example-validate COMMAND ${Python3_EXECUTABLE} t.py)\n'
                    'endif()\n')
        self.commit("test: example-only validator")
        missing, advisories = self.run_check()
        self.assertEqual(missing, [])
        self.assertEqual(len(advisories), 1)
        self.assertIn("gate inventory", advisories[0])

    def test_a_listed_neighbour_makes_its_branch_checked(self) -> None:
        self.write(MANIFEST, 'if(PULP_HAS_GPU)\n'
                             '  add_test(NAME gpu-listed COMMAND ${Python3_EXECUTABLE} a.py)\n'
                             'elseif(UNIX)\n'
                             '  add_test(NAME other-branch COMMAND ${Python3_EXECUTABLE} b.py)\n'
                             'endif()\n')
        self.inputs({"gpu-listed"})
        self.commit("seed a gated block")
        self.git("branch", "-f", "main", "HEAD")
        self.write(MANIFEST, 'if(PULP_HAS_GPU)\n'
                             '  add_test(NAME gpu-listed COMMAND ${Python3_EXECUTABLE} a.py)\n'
                             '  add_test(NAME gpu-new COMMAND ${Python3_EXECUTABLE} c.py)\n'
                             'elseif(UNIX)\n'
                             '  add_test(NAME other-branch COMMAND ${Python3_EXECUTABLE} b.py)\n'
                             '  add_test(NAME other-new COMMAND ${Python3_EXECUTABLE} d.py)\n'
                             'endif()\n')
        self.commit("add one test to each branch")
        missing, advisories = self.run_check()
        self.assertEqual([line.split(": ")[1] for line in missing], ["gpu-new"])
        self.assertEqual(len(advisories), 1)
        self.assertIn("other-new", advisories[0])

    def test_a_python_only_guard_shares_the_enclosing_scope(self) -> None:
        self.append('if(Python3_Interpreter_FOUND)\n'
                    '  add_test(NAME guarded-new COMMAND ${Python3_EXECUTABLE} t.py)\n'
                    'endif()\n')
        self.commit("test: python-guarded")
        missing, _ = self.run_check()
        self.assertEqual([line.split(": ")[1] for line in missing], ["guarded-new"])

    def test_a_failure_names_the_regenerate_command_and_the_exclusion(self) -> None:
        self.append('add_test(NAME new-selftest COMMAND ${Python3_EXECUTABLE} t.py)\n')
        self.commit("test: add new-selftest")
        err = io.StringIO()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
            self.assertEqual(precheck.main(["--root", str(self.root), "--base", "main"]), 1)
        self.assertIn("script_test_inputs.py --build-dir build --write", err.getvalue())
        self.assertIn("# script-inputs: not-gated <reason>", err.getvalue())

    def test_advisories_are_never_reported_as_a_plain_ok(self) -> None:
        self.append('add_test(NAME ${_case}-selftest COMMAND ${Python3_EXECUTABLE} t.py)\n')
        self.commit("test: computed name")
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(precheck.main(["--root", str(self.root), "--base", "main"]), 0)
        self.assertIn("ok with 1 not checked", out.getvalue())

    def test_an_unreadable_base_is_not_a_pass(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(precheck.main(["--root", str(self.root), "--base", "nope"]), 2)


class RepositoryTests(unittest.TestCase):
    def test_no_ungated_test_in_the_repository_would_fail(self) -> None:
        """Every script-driven test the gate inventory leaves out (examples,
        GPU and scene3d probes, platform-only tests), judged as if it were new,
        is NOT CHECKED rather than failed: regenerating cannot add it."""
        import cmake_registration_impact as registrations

        listed = set(json.loads((REPO / precheck.INPUTS_LIST).read_text(encoding="utf-8"))["tests"])
        files = [path for path in subprocess.run(
            ["git", "-C", str(REPO), "ls-files"], capture_output=True, text=True,
            encoding="utf-8", check=True).stdout.splitlines() if precheck.is_cmake_file(path)]
        unlisted = 0
        failures: list[str] = []
        for path in files:
            text = (REPO / path).read_text(encoding="utf-8", errors="replace")
            names = {precheck._name(call) for call in registrations.calls(text)
                     if call.command == "add_test"} - listed
            missing, advisories = precheck.judge(path, text, names, listed, REPO)
            unlisted += len(missing) + len(advisories)
            failures.extend(missing)
        self.assertGreater(unlisted, 100, "the negative set must actually be exercised")
        self.assertEqual(failures, [])


if __name__ == "__main__":
    unittest.main()
