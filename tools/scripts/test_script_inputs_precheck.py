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

    def test_shell_node_env_wrapped_and_direct_scripts_count(self) -> None:
        self.append(
            'add_test(NAME sh-test COMMAND bash "${CMAKE_SOURCE_DIR}/tools/scripts/t.sh")\n'
            'add_test(NAME node-test COMMAND ${_PULP_NODE_FOR_TESTS} "${CMAKE_SOURCE_DIR}/x.test.mjs")\n'
            'add_test(NAME env-test COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 python3\n'
            '    ${CMAKE_SOURCE_DIR}/tools/scripts/b.py --repo ${CMAKE_SOURCE_DIR})\n'
            'add_test(NAME direct-test COMMAND ${CMAKE_SOURCE_DIR}/tools/scripts/run.sh)\n')
        self.commit("test: four script tests")
        missing, _ = self.run_check()
        self.assertEqual(sorted(line.split(": ")[1] for line in missing),
                         ["direct-test", "env-test", "node-test", "sh-test"])

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

    def test_an_unreadable_base_is_not_a_pass(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(precheck.main(["--root", str(self.root), "--base", "nope"]), 2)


if __name__ == "__main__":
    unittest.main()
