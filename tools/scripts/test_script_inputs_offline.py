#!/usr/bin/env python3
"""Tests for script_inputs_offline.py against throwaway git ranges.

Each regression replays the shape of a stale fragment that reached the
required macOS gate: a script selftest that started importing its module, a
new test suite with no executables/ fragment, and a spawning source that joined
a compiled target. The negative controls prove the check selects and
recomputes an entry and still passes when its fragment is current.
"""

from __future__ import annotations

import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import script_inputs_offline as offline  # noqa: E402
import script_inputs_store as store  # noqa: E402

SELFTEST = "tools/scripts/test_cutover_trial.py"
SELFTEST_SEED = '"""Selftest."""\nimport unittest\n\n\nclass T(unittest.TestCase):\n    pass\n'
QUALITY = "test/cmake/quality_tests.cmake"
SUITES = "test/cmake/trigger_tests.cmake"
CLI_TESTS = "test/cmake/cli_tests.cmake"
CLI = "tools/cli/CMakeLists.txt"
DEPENDENCIES = "tools/cmake/PulpDependencies.cmake"


class OfflineTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.git("init", "-q", "-b", "main")
        for key, value in (("user.email", "t@e"), ("user.name", "t"),
                           ("commit.gpgsign", "false")):
            self.git("config", key, value)
        # A Python selftest and the files a later edit makes it name.
        self.write(SELFTEST, SELFTEST_SEED)
        self.write("tools/scripts/cutover_trial.py", "VALUE = 1\n")
        self.write("tools/ci/governed-build.sh", "#!/bin/sh\n")
        self.write("tools/mcp/server.py", "\n")
        self.write(QUALITY,
                   "add_test(NAME cutover-trial-selftest COMMAND ${Python3_EXECUTABLE}\n"
                   f"    ${{CMAKE_SOURCE_DIR}}/{SELFTEST})\n"
                   "set_tests_properties(cutover-trial-selftest PROPERTIES TIMEOUT 120)\n")
        # A compiled suite under a guard the gate evidently enters.
        self.write(DEPENDENCIES, "FetchContent_MakeAvailable(clap)\n")
        self.write(SUITES, "if(TARGET pulp-host)\n"
                           "    pulp_add_test_suite(pulp-test-trigger-graph\n"
                           "        SOURCES test_trigger_graph.cpp LIBRARIES pulp::host)\n"
                           "endif()\n")
        self.write("test/test_trigger_graph.cpp", "int x = 0;\n")
        # The CLI target (run by ctest as pulp-cpp) and a suite that shells out.
        self.write(CLI, "add_executable(pulp-cli\n    cli_main.cpp\n    cli_sdk.cpp\n)\n")
        self.write("tools/cli/cli_main.cpp", "int main() { return 0; }\n")
        self.write("tools/cli/cli_sdk.cpp", "void run() { ChildProcess p; }\n")
        # This source is present on the base branch but is not in the target;
        # a later CMake-only list edit must still inspect its spawn signal.
        self.write("tools/cli/cli_watch_shell.cpp",
                   "void watch() { ChildProcess shell; }\n")
        self.write(CLI_TESTS, "add_executable(pulp-test-cli-shellout-loop "
                              "test_cli_shellout_loop.cpp)\n")
        self.write("test/test_cli_shellout_loop.cpp", "int y = 0;\n")
        self.write("README.md", "seed\n")
        # The include chains: the test tree's manifests, and the dependency
        # module the root configure includes unconditionally.
        self.write("test/CMakeLists.txt", "".join(
            f'include("${{CMAKE_CURRENT_SOURCE_DIR}}/cmake/{Path(m).name}")\n'
            for m in (QUALITY, SUITES, CLI_TESTS)))
        self.write("CMakeLists.txt", "include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/"
                                     "PulpDependencies.cmake)\nadd_subdirectory(test)\n")
        doc = {
            "schema": "pulp-ctest-script-inputs/v1",
            "tests": {"cutover-trial-selftest": {
                "entry": SELFTEST, "inputs": [SELFTEST], "kind": "python"}},
            "executables_scanned_for": ["data", "spawns"],
            "executables_scanned": ["pulp-cpp", "pulp-test-cli-shellout-loop",
                                    "pulp-test-trigger-graph"],
            "executable_targets": {"pulp-cpp": "pulp-cli"},
            "executables": {"pulp-cpp": {
                "data": "none", "inputs": [], "kind": "compiled", "runtime_targets": [],
                "sources": [], "spawning_sources": ["tools/cli/cli_sdk.cpp"],
                "spawns": "declared", "undeclared_sources": []}},
        }
        store.write_path(self.root / store.FRAGMENT_DIR, doc)
        self.commit("seed")
        self.git("checkout", "-q", "-b", "feature")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    # -- helpers -----------------------------------------------------------
    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.root), *args], capture_output=True,
                              text=True, encoding="utf-8", check=True).stdout.strip()

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def append(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.write_text(path.read_text(encoding="utf-8") + text, encoding="utf-8")

    def commit(self, message: str) -> None:
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)

    def run_main(self, *extra: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = offline.main(["--root", str(self.root), "--base", "main", *extra])
        return rc, out.getvalue(), err.getvalue()

    def fragment(self, rel: str) -> str:
        return (self.root / store.FRAGMENT_DIR / rel).read_text(encoding="utf-8")

    # -- regressions -------------------------------------------------------
    def test_newly_resolved_import_selects_unchanged_importer(self) -> None:
        self.append(SELFTEST, "import newly_available\n")
        self.commit("test: unresolved import")
        self.git("branch", "-f", "main", "HEAD")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)
        self.write("tools/scripts/newly_available.py", "VALUE = 1\n")
        self.commit("test: add the imported module")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 1, err)
        self.assertIn('+   "tools/scripts/newly_available.py",', err)
        rc, _, err = self.run_main("--write")
        self.assertEqual(rc, 0, err)
        self.commit("test: record the new import")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)

    def test_unused_source_list_does_not_prove_target_membership(self) -> None:
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)
        self.append(CLI, "set(_unused cli_sdk.cpp new_spawn.cpp)\n")
        self.write("tools/cli/new_spawn.cpp", "void run() { ChildProcess child; }\n")
        self.commit("test: unused source list")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)
        self.assertIn("NOT CHECKED pulp-cpp: tools/cli/new_spawn.cpp", err)
        self.assertNotIn("fragment(s) this push makes stale", err)

    def test_unrelated_module_does_not_blame_existing_stale_imports(self) -> None:
        self.append(SELFTEST, "import cutover_trial\n")
        self.commit("test: existing stale import")
        self.git("branch", "-f", "main", "HEAD")
        self.write("tools/scripts/unrelated.py", "VALUE = 1\n")
        self.commit("test: unrelated module")
        report = offline.check(self.root, "main")
        self.assertEqual(report.checked, 0)
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)

    def test_cmake_only_source_list_addition_needs_configure(self) -> None:
        self.write(CLI, "add_executable(pulp-cli\n    cli_main.cpp\n    cli_sdk.cpp\n"
                       "    cli_watch_shell.cpp\n)\n")
        self.commit("test: add existing spawner to target")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 1, err)
        self.assertIn("executables/pulp-cpp.json (pulp-cpp)", err)
        self.assertIn("tools/cli/cli_watch_shell.cpp calls a process", err)

    def test_cmake_only_source_list_removal_needs_configure(self) -> None:
        self.write(CLI, "add_executable(pulp-cli\n    cli_main.cpp\n)\n")
        self.commit("test: remove recorded spawner from target")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 1, err)
        self.assertIn("executables/pulp-cpp.json (pulp-cpp)", err)
        self.assertIn("tools/cli/cli_sdk.cpp was removed from a CMake source list", err)

    def test_cmake_list_variable_is_not_silently_truncated(self) -> None:
        self.write(QUALITY,
                   "set(_trial_args \"tools/ci/governed-build.sh;tools/mcp/server.py\")\n"
                   "add_test(NAME cutover-trial-selftest COMMAND ${Python3_EXECUTABLE}\n"
                   f"    ${{CMAKE_SOURCE_DIR}}/{SELFTEST} ${{_trial_args}})\n")
        self.commit("test: use a multi-value CMake list")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)
        self.assertIn("NOT CHECKED cutover-trial-selftest", err)
        self.assertIn("multi-value CMake list", err)

    def test_newly_resolved_transitive_import_selects_unchanged_importer(self) -> None:
        self.append(SELFTEST, "import cutover_trial\n")
        self.append("tools/scripts/cutover_trial.py", "from . import newly_available\n")
        self.commit("test: unresolved transitive relative import")
        rc, _, err = self.run_main("--write")
        self.assertEqual(rc, 0, err)
        self.commit("test: record existing imports")
        self.git("branch", "-f", "main", "HEAD")
        self.write("tools/scripts/newly_available.py", "VALUE = 1\n")
        self.commit("test: resolve transitive import")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 1, err)
        self.assertIn('+   "tools/scripts/newly_available.py",', err)

    def test_selftest_that_starts_importing_its_module_is_stale(self) -> None:
        self.write(SELFTEST, SELFTEST_SEED + "import cutover_trial\n"
                   'GOVERNOR = "tools/ci/governed-build.sh"\nMCP = "tools/mcp"\n')
        self.commit("test: exercise the trial module")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 1, err)
        self.assertIn("+++ b/test/ctest_script_inputs/tests/cutover-trial-selftest.json", err)
        for path in ("tools/scripts/cutover_trial.py", "tools/ci/governed-build.sh", "tools/mcp"):
            self.assertIn(f'+   "{path}",', err)
        self.assertIn("script_inputs_offline.py --base main --write", err)
        # The printed fix makes the next run pass.
        rc, out, _ = self.run_main("--write")
        self.assertEqual(rc, 0)
        self.assertIn("wrote test/ctest_script_inputs/tests/cutover-trial-selftest.json", out)
        self.assertIn('"tools/scripts/cutover_trial.py"',
                      self.fragment("tests/cutover-trial-selftest.json"))
        self.commit("test: regenerate the selftest's inputs")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)

    def test_new_suite_without_an_executables_fragment_is_missing(self) -> None:
        self.append(SUITES, "if(TARGET pulp-host AND TARGET clap)\n"
                            "    pulp_add_test_suite(pulp-test-g5-host-event-timestamps\n"
                            "        SOURCES test_g5_host_event_timestamps.cpp "
                            "LIBRARIES pulp::host clap)\n"
                            "endif()\n")
        self.write("test/test_g5_host_event_timestamps.cpp", "int z = 0;\n")
        self.commit("test: add the host timestamp suite")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 1, err)
        self.assertIn("+++ b/test/ctest_script_inputs/executables/"
                      "pulp-test-g5-host-event-timestamps.json", err)
        self.assertIn('+ "scanned": true', err)
        rc, _, _ = self.run_main("--write")
        self.assertEqual(rc, 0)
        self.assertEqual(self.fragment("executables/pulp-test-g5-host-event-timestamps.json"),
                         store.dumps({"name": "pulp-test-g5-host-event-timestamps",
                                      "scanned": True}))
        self.commit("test: record the suite")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)

    def test_spawning_source_joining_the_cli_target_needs_configure(self) -> None:
        self.write(CLI, "add_executable(pulp-cli\n    cli_main.cpp\n    cli_sdk.cpp\n"
                        "    cli_watch_shell.cpp\n)\n")
        self.write("tools/cli/cli_watch_shell.cpp",
                   "// Runs the watch command.\nvoid watch() { ChildProcess shell; }\n")
        self.commit("feat: watch through a shell")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 1, err)
        self.assertIn("executables/pulp-cpp.json (pulp-cpp)", err)
        self.assertIn("tools/cli/cli_watch_shell.cpp calls a process or loader API", err)
        self.assertIn("script_test_inputs.py --build-dir", err)

    def test_suite_source_that_starts_spawning_needs_configure(self) -> None:
        self.write("test/test_cli_shellout_loop.cpp",
                   "int y = 0;\nvoid probe() { ChildProcess child; }\n")
        self.commit("test: drive the loop through a child process")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 1, err)
        self.assertIn("executables/pulp-test-cli-shellout-loop.json", err)

    # -- negative controls -------------------------------------------------
    def test_current_fragment_passes_after_being_recomputed(self) -> None:
        # The selftest changes without changing what it imports or names, so
        # the check selects it, recomputes it, and finds the fragment current.
        self.write(SELFTEST, SELFTEST_SEED + "# a comment\n")
        self.write("README.md", "changed\n")
        self.commit("docs: touch the selftest")
        report = offline.check(self.root, "main")
        self.assertEqual((report.stale, report.needs_configure), ([], []))
        self.assertEqual(report.checked, 1)
        rc, out, _ = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("1 recomputed", out)

    def test_unrelated_change_selects_nothing(self) -> None:
        self.write("README.md", "changed\n")
        self.commit("docs: readme")
        report = offline.check(self.root, "main")
        self.assertEqual((report.stale, report.needs_configure, report.checked), ([], [], 0))

    def test_non_spawning_source_joining_the_cli_target_passes(self) -> None:
        self.write(CLI, "add_executable(pulp-cli\n    cli_main.cpp\n    cli_sdk.cpp\n"
                        "    cli_quote.cpp\n)\n")
        self.write("tools/cli/cli_quote.cpp", "int quote() { return 0; }\n")
        self.commit("feat: quoting")
        report = offline.check(self.root, "main")
        self.assertEqual((report.stale, report.needs_configure), ([], []))
        self.assertGreater(report.checked, 0)

    # -- what it cannot see is said, never passed silently --------------------
    def test_command_known_only_after_configure_is_not_checked(self) -> None:
        self.write(QUALITY,
                   "add_test(NAME cutover-trial-selftest COMMAND ${Python3_EXECUTABLE}\n"
                   f"    ${{CMAKE_SOURCE_DIR}}/{SELFTEST} ${{_trial_args}})\n")
        self.commit("test: pass configure-time arguments")
        rc, out, err = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("NOT CHECKED cutover-trial-selftest", err)
        self.assertIn("${_trial_args}", err)
        self.assertIn("1 not checked", out)

    def test_suite_under_an_unknown_guard_is_not_checked(self) -> None:
        self.append(SUITES, "if(PULP_ENABLE_GPU_PROOF)\n"
                            "    pulp_add_test_suite(pulp-test-gpu-proof SOURCES test_gpu.cpp)\n"
                            "endif()\n")
        self.write("test/test_gpu.cpp", "int g = 0;\n")
        self.commit("test: add a proof suite")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("NOT CHECKED test/cmake/trigger_tests.cmake:6: pulp-test-gpu-proof", err)
        self.assertIn("if(PULP_ENABLE_GPU_PROOF)", err)

    def test_not_gated_marker_excludes_a_new_suite(self) -> None:
        self.append(SUITES, "# script-inputs: not-gated hardware only\n"
                            "pulp_add_test_suite(pulp-test-hw SOURCES test_hw.cpp)\n")
        self.write("test/test_hw.cpp", "int h = 0;\n")
        self.commit("test: hardware suite")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)
        self.assertNotIn("pulp-test-hw", err)

    def test_generator_change_is_named(self) -> None:
        self.write("tools/scripts/script_test_inputs.py", "# generator\n")
        self.commit("chore: touch the generator")
        _, _, err = self.run_main()
        self.assertIn("NOT CHECKED tools/scripts/script_test_inputs.py changed", err)

    def test_pushing_a_commit_other_than_head_is_not_checked(self) -> None:
        tip = self.git("rev-parse", "HEAD")
        self.write("README.md", "next\n")
        self.commit("docs: next")
        report = offline.check(self.root, "main", tip)
        self.assertEqual(report.checked, 0)
        self.assertIn("is not the checked-out HEAD", report.not_checked[0])

    def test_removed_suite_with_a_remaining_fragment_needs_configure(self) -> None:
        self.write(SUITES, "# no suites\n")
        self.commit("test: drop the trigger graph suite")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 1, err)
        self.assertIn("executables/pulp-test-trigger-graph.json (pulp-test-trigger-graph): "
                      f"{SUITES} no longer defines it", err)

    # -- review findings: no false fail, no silent pass -----------------------
    def test_standalone_consumer_project_is_not_demanded_a_fragment(self) -> None:
        self.write("test/cmake/sdk_consumer.cmake", "cmake_minimum_required(VERSION 3.25)\n"
                   "project(consumer CXX)\nadd_executable(consumer main.cpp)\n")
        self.commit("test: an installed-SDK consumer project")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)
        self.assertNotIn("consumer.json", err)
        self.assertIn("NOT CHECKED test/cmake/sdk_consumer.cmake: a new executable here", err)

    def test_variable_set_inside_a_branch_is_not_expanded(self) -> None:
        self.write(QUALITY,
                   "if(WIN32)\n    set(_trial_extra ${CMAKE_SOURCE_DIR}/tools/mcp/server.py)\n"
                   "endif()\n"
                   "add_test(NAME cutover-trial-selftest COMMAND ${Python3_EXECUTABLE}\n"
                   f"    ${{CMAKE_SOURCE_DIR}}/{SELFTEST} ${{_trial_extra}})\n")
        self.commit("test: a platform-only argument")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)
        self.assertIn("NOT CHECKED cutover-trial-selftest", err)
        self.assertIn("${_trial_extra}", err)

    def test_pushed_commit_with_an_uncommitted_fix_is_not_checked(self) -> None:
        self.write(SELFTEST, SELFTEST_SEED + "import cutover_trial\n")
        self.commit("test: import the trial module")
        self.assertEqual(self.run_main("--write")[0], 0)  # written, never committed
        head = self.git("rev-parse", "HEAD")
        rc, _, err = self.run_main("--head", head)
        self.assertEqual(rc, 0)
        self.assertIn("NOT CHECKED uncommitted edits to test/ctest_script_inputs/", err)
        self.git("checkout", "--", ".")
        rc, _, err = self.run_main("--head", head)
        self.assertEqual(rc, 1, err)

    def test_header_that_starts_spawning_is_not_checked(self) -> None:
        self.write("test/support/loop_probe.hpp", "#pragma once\n")
        self.commit("test: add a probe header")
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--ff-only", "feature")
        self.git("checkout", "-q", "feature")
        self.write("test/support/loop_probe.hpp", "#pragma once\ninline void p() { ChildProcess c; }\n")
        self.commit("test: the probe header starts a child")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)
        self.assertIn("NOT CHECKED test/support/loop_probe.hpp now calls a process", err)

    def test_target_fetched_only_under_a_platform_guard_is_not_trusted(self) -> None:
        self.write("tools/cmake/PulpAndroid.cmake", "FetchContent_MakeAvailable(oboe)\n")
        self.write("CMakeLists.txt", "include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/"
                                     "PulpDependencies.cmake)\nif(ANDROID)\n    include("
                                     "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAndroid.cmake)\n"
                                     "endif()\nadd_subdirectory(test)\n")
        self.append(SUITES, "if(TARGET oboe)\n"
                            "    pulp_add_test_suite(pulp-test-oboe SOURCES test_oboe.cpp)\n"
                            "endif()\n")
        self.write("test/test_oboe.cpp", "int o = 0;\n")
        self.commit("test: an Android-only suite")
        rc, _, err = self.run_main()
        self.assertEqual(rc, 0, err)
        self.assertIn("pulp-test-oboe has no fragment, but it sits under if(TARGET oboe)", err)


if __name__ == "__main__":
    unittest.main()
