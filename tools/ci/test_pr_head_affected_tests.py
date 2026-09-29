#!/usr/bin/env python3
"""Tests for tools/ci/pr_head_affected_tests.py."""

from __future__ import annotations

import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import pr_head_affected_tests as step  # noqa: E402

REPO = HERE.parent.parent
BUILD_YML = REPO / ".github" / "workflows" / "build.yml"


def selection(mode: str, tests: list[str], reason: str = "r") -> step.inventory.Selection:
    return step.inventory.Selection(mode=mode, reason=reason, changed_files=[], targets=[],
                                    tests=tests, total_targets=0, total_tests=0,
                                    unmapped=[], threshold=0.0)


NAMES = {
    "unit-a": set(), "unit-b": set(), "fast-one": {"pr-fast"},
    "cmake-install-layout": set(), "cmake-sdk-smoke": set(),
    "wide-non-native-selftest": set(), "other": set(),
}


class SelectionTests(unittest.TestCase):
    def plan(self, paths: list[str], projected: step.inventory.Selection) -> dict:
        with mock.patch.object(step, "project", return_value=projected) as project:
            plan = step.select(Path("/b"), paths, NAMES)
        plan["_projected"] = project.called
        return plan

    def test_a_focused_projection_selects_its_tests_minus_the_fast_tier(self) -> None:
        plan = self.plan(["core/x/a.cpp"], selection("focused", ["unit-a", "fast-one", "gone"]))
        self.assertEqual(plan["tests"], ["unit-a"])

    def test_an_all_fallback_selects_nothing_on_a_pull_request_head(self) -> None:
        plan = self.plan(["core/x/a.cpp"], selection("all", ["unit-a", "unit-b"], "unmapped"))
        self.assertEqual((plan["mode"], plan["tests"]), ("all", []))

    def test_a_docs_workflow_only_diff_selects_nothing_without_projecting(self) -> None:
        plan = self.plan([".github/workflows/x.yml", "docs/a.md", ".agents/skills/ci/SKILL.md",
                          "README.md"], selection("focused", ["unit-a"]))
        self.assertEqual(plan["tests"], [])
        self.assertFalse(plan["_projected"])

    def test_control_one_code_path_is_enough_to_project(self) -> None:
        plan = self.plan(["docs/a.md", "core/x/a.cpp"], selection("focused", ["unit-a"]))
        self.assertEqual(plan["tests"], ["unit-a"])

    def test_tools_cmake_reaches_the_cmake_fixtures(self) -> None:
        plan = self.plan(["tools/cmake/PulpPlugin.cmake"], selection("all", []))
        self.assertEqual(sorted(plan["tests"]), ["cmake-install-layout", "cmake-sdk-smoke"])

    def test_the_tier_manifest_classifier_and_registrations_reach_wide_non_native(self) -> None:
        for path in ("tools/ci/wide_non_native_checks.json", "tools/scripts/classify_changes.py",
                     "test/cmake/cmake_smoke_tests.cmake", "test/CMakeLists.txt"):
            with self.subTest(path=path):
                plan = self.plan([path], selection("focused", []))
                self.assertIn("wide-non-native-selftest", plan["tests"])

    def test_an_unrelated_tools_change_does_not_reach_the_families(self) -> None:
        plan = self.plan(["tools/scripts/other.py"], selection("focused", ["other"]))
        self.assertEqual(plan["tests"], ["other"])


class BudgetTests(unittest.TestCase):
    """Batches stop starting once the budget is spent; leftovers are skipped."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        # A ctest stand-in: reads the listed tests and writes a JUnit file in
        # which a test named `bad-*` fails and the rest pass.
        (self.bin / "ctest").write_text(
            "#!/usr/bin/env python3\n"
            "import sys, time\n"
            "a = sys.argv\n"
            "names = open(a[a.index('--tests-from-file') + 1]).read().split()\n"
            "time.sleep(float(__import__('os').environ.get('FAKE_CTEST_SLEEP', '0')))\n"
            "cases = ''.join(f'<testcase name=\"{n}\" status=\"{\"fail\" if n.startswith(\"bad\") else \"run\"}\">'\n"
            "                + ('<failure/>' if n.startswith('bad') else '') + '</testcase>' for n in names)\n"
            "open(a[a.index('--output-junit') + 1], 'w').write(f'<testsuite>{cases}</testsuite>')\n")
        (self.bin / "ctest").chmod(0o755)
        self.env = mock.patch.dict(os.environ, {"PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}"})
        self.env.start()

    def tearDown(self) -> None:
        self.env.stop()
        self._tmp.cleanup()

    def test_failures_are_reported_and_passes_counted(self) -> None:
        results = step.run_budgeted(self.tmp, ["a", "bad-b", "c"], 60, 1, "", self.tmp, 2)
        self.assertEqual(results["failed"], ["bad-b"])
        self.assertEqual(sorted(results["passed"]), ["a", "c"])
        self.assertEqual(results["skipped"], [])

    def test_no_batch_starts_after_the_budget(self) -> None:
        with mock.patch.dict(os.environ, {"FAKE_CTEST_SLEEP": "0.3"}):
            results = step.run_budgeted(self.tmp, ["a", "b", "c", "d"], 0.2, 1, "", self.tmp, 2)
        self.assertEqual(sorted(results["passed"]), ["a", "b"])
        self.assertEqual(results["skipped"], ["c", "d"])


class WorkflowWiringTests(unittest.TestCase):
    def test_the_step_gates_the_pull_request_head_after_the_fast_tier(self) -> None:
        text = BUILD_YML.read_text(encoding="utf-8")
        fast = text.index("- name: Test fast deterministic tier (pull request head)")
        step_at = text.index("- name: Test what this pull request reaches (pull request head)")
        full = text.index("- name: Test (non-Windows)")
        self.assertLess(fast, step_at)
        self.assertLess(step_at, full)
        block = text[step_at:full]
        self.assertIn("github.event_name == 'pull_request'", block)
        self.assertIn("tools/ci/pr_head_affected_tests.py", block)
        self.assertIn("--budget-secs 600", block)
        self.assertIn("--base HEAD^1 --head HEAD", block)
        # Gating: a failure here must fail the required check, while a selector
        # that could not read its inputs is named as such rather than a verdict.
        self.assertNotIn("continue-on-error", block)
        self.assertIn("1) exit 1 ;;", block)
        self.assertRegex(block, r"2\) echo \"::warning")

    def test_configure_asks_cmake_for_the_codemodel(self) -> None:
        text = BUILD_YML.read_text(encoding="utf-8")
        configure = text[text.index("      - name: Configure\n"):]
        configure = configure[:configure.index("\n      - name:", 10)]
        query = configure.index(".cmake/api/v1/query/codemodel-v2")
        self.assertLess(query, configure.index('cmake -S . -B "$PULP_BUILD_DIR"'))


class BaseRedTests(unittest.TestCase):
    """Only a script test that fails the same way on the base is exempted."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name).resolve()
        self.build = self.tmp / "build"
        (self.build / "test").mkdir(parents=True)
        binary = self.build / "test" / "pulp-test-x"
        binary.write_text("")
        inventory = {"tests": [
            {"name": "script-red", "command": ["/usr/bin/python3", str(REPO / "tools/scripts/a.py")]},
            {"name": "script-flake", "command": ["/usr/bin/python3", str(REPO / "tools/scripts/b.py")]},
            {"name": "script-new", "command": ["/usr/bin/python3", str(REPO / "tools/scripts/c.py")]},
            {"name": "compiled", "command": [str(binary), "Some case"]},
        ]}
        bindir = self.tmp / "bin"
        bindir.mkdir()
        (bindir / "ctest").write_text(f"#!/bin/sh\ncat {self.tmp / 'inv.json'}\n")
        (bindir / "ctest").chmod(0o755)
        (self.tmp / "inv.json").write_text(__import__("json").dumps(inventory))
        self.env = mock.patch.dict(os.environ, {"PATH": f"{bindir}{os.pathsep}{os.environ['PATH']}"})
        self.env.start()
        self.lane = mock.MagicMock()
        self.lane._entry_from_command.side_effect = (
            lambda name, argv, props, repo, build: {"name": name, "raw": True, "argv": [
                a.replace(str(repo), "{repo}") for a in argv]})
        self.lane.run.side_effect = lambda entries, **kw: [
            {"name": e["name"], "returncode": 0 if e["name"] == "script-flake" else 1,
             "output": "FAIL: t (m.T.t)\n"} for e in entries]
        self.lane.rerun_on_base.side_effect = lambda still, entries, ref: {
            r["name"]: (r["name"] == "script-red", "why") for r in still}
        self.modules = mock.patch.dict(sys.modules, {"source_selftests": self.lane})
        self.modules.start()

    def tearDown(self) -> None:
        self.modules.stop()
        self.env.stop()
        self._tmp.cleanup()

    def test_only_a_script_test_red_on_the_base_is_exempted(self) -> None:
        verdicts = step.label_base_failures(
            self.build, ["script-red", "script-flake", "script-new", "compiled"], "HEAD^1")
        self.assertEqual({k: v[0] for k, v in verdicts.items()},
                         {"script-red": True, "script-flake": False,
                          "script-new": False, "compiled": False})
        self.assertIn("compiled test", verdicts["compiled"][1])
        self.assertIn("flake", verdicts["script-flake"][1])
        # The base re-run is asked only about scripts still failing here.
        still = self.lane.rerun_on_base.call_args[0][0]
        self.assertEqual(sorted(r["name"] for r in still), ["script-new", "script-red"])


class ExitCodeTests(unittest.TestCase):
    """A red main does not fail the pull request; its own failure does."""

    def _main(self, verdicts: dict) -> int:
        plan = {"changed": 1, "mode": "focused", "reason": "r", "families": {},
                "tests": ["a", "b"]}
        results = {"passed": [], "failed": list(verdicts), "skipped": [],
                   "excluded_or_absent": [], "seconds": 1.0}
        with mock.patch.object(step, "diff_paths", return_value=["x.py"]), \
                mock.patch.object(step, "inventory_tests", return_value={}), \
                mock.patch.object(step, "select", return_value=plan), \
                mock.patch.object(step, "run_budgeted", return_value=results), \
                mock.patch.object(step, "label_base_failures", return_value=verdicts), \
                contextlib.redirect_stdout(io.StringIO()):
            return step.main(["--build-dir", "/nonexistent"])

    def test_every_failure_pre_existing_passes(self) -> None:
        self.assertEqual(self._main({"a": (True, "same on base")}), 0)

    def test_control_one_branch_caused_failure_fails(self) -> None:
        self.assertEqual(self._main({"a": (True, "same"), "b": (False, "passes on base")}), 1)


if __name__ == "__main__":
    unittest.main()
