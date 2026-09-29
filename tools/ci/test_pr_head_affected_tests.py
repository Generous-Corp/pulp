#!/usr/bin/env python3
"""Tests for tools/ci/pr_head_affected_tests.py."""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import cmake_registration_impact as impact  # noqa: E402
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
    "pulp-gpu-host-mapped-pointer-probe": set(), "pulp-gpu-audio-provider-identity": set(),
    "pulp-gpu-dawn-shared-io-provider-probe": set(), "pulp-gpu-compute-unrelated": set(),
}


class SelectionTests(unittest.TestCase):
    def plan(self, paths: list[str], projected: step.inventory.Selection,
             *, scripts: list[str] = (), inputs: list[str] = (), own: list[str] = ()) -> dict:
        with mock.patch.object(step, "project", return_value=projected) as project, \
                mock.patch.object(step, "script_reference_tests", return_value=list(scripts)), \
                mock.patch.object(step, "script_input_tests", return_value=list(inputs)), \
                mock.patch.object(step, "own_tests", return_value=list(own)), \
                mock.patch.object(step, "registration_tests", return_value=[]):
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

    def test_a_provider_identity_producer_reaches_the_provider_probes(self) -> None:
        probes = ["pulp-gpu-audio-provider-identity", "pulp-gpu-dawn-shared-io-provider-probe",
                  "pulp-gpu-host-mapped-pointer-probe"]
        for path in ("core/gpu_audio/CMakeLists.txt", "tools/cmake/PulpGpuAudioProviderIdentity.cmake",
                     "tools/scripts/gpu_audio_provider_identity.py", "tools/deps/manifest.json"):
            with self.subTest(path=path):
                plan = self.plan([path], selection("all", []))
                self.assertEqual(sorted(t for t in plan["tests"] if t.startswith("pulp-gpu")), probes)

    def test_control_other_gpu_audio_code_does_not_reach_the_provider_probes(self) -> None:
        plan = self.plan(["core/gpu_audio/src/other.cpp"], selection("focused", []))
        self.assertEqual(plan["tests"], [])

    def test_script_tests_are_added_even_for_a_docs_only_diff(self) -> None:
        # A census test parses docs/status: the declared inputs apply where the
        # build projection does not run at all.
        plan = self.plan(["docs/status/consumption-profiles.json"], selection("focused", []),
                         inputs=["unit-a"], scripts=["unit-b"])
        self.assertEqual(sorted(plan["tests"]), ["unit-a", "unit-b"])
        self.assertFalse(plan["_projected"])

    def test_a_too_wide_projection_still_runs_the_tests_the_diff_edits(self) -> None:
        plan = self.plan(["core/x/a.cpp", "test/test_a.cpp"],
                         selection("all", [], "670 affected targets exceed 40% of 774"),
                         own=["unit-a"])
        self.assertEqual(plan["tests"], ["unit-a"])

    def test_control_a_focused_projection_does_not_add_own_tests_twice(self) -> None:
        plan = self.plan(["test/test_a.cpp"], selection("focused", ["unit-a"]), own=["unit-b"])
        self.assertEqual(plan["tests"], ["unit-a"])

    def test_an_unrelated_tools_change_does_not_reach_the_families(self) -> None:
        plan = self.plan(["tools/scripts/other.py"], selection("focused", ["other"]))
        self.assertEqual(plan["tests"], ["other"])


MANIFEST = """\
add_executable(pulp-probe probe.cpp)
foreach(key sha dir)
    get_target_property(_provider_id_${key} pulp-lib "PROVIDER_${key}")
endforeach()
target_compile_definitions(pulp-probe PRIVATE
    SHA="${_provider_id_sha}")
add_test(NAME probe-run COMMAND pulp-probe)
# a comment that names nothing
add_test(NAME unrelated COMMAND other)
set_tests_properties(unrelated PROPERTIES TIMEOUT 5)
if(_arm_only)
    add_test(NAME arm-control COMMAND pulp-probe --arm)
endif()
"""


class RegistrationImpactTests(unittest.TestCase):
    def reach(self, touched: set[int], removed: list[str] = ()) -> impact.Impact:
        return impact.impact(MANIFEST, touched, removed)

    def test_an_edited_add_test_reaches_that_test_only(self) -> None:
        self.assertEqual(self.reach({7}).tests, {"probe-run"})

    def test_an_edited_properties_call_reaches_its_tests(self) -> None:
        self.assertEqual(self.reach({10}).tests, {"unrelated"})

    def test_a_templated_variable_write_reaches_the_target_that_reads_it(self) -> None:
        # The loop body writes _provider_id_${key}; the probe's definitions read it.
        reached = self.reach({3})
        self.assertIn("pulp-probe", reached.targets)
        self.assertNotIn("unrelated", reached.tests)

    def test_a_removed_variable_write_reaches_the_block_it_conditions(self) -> None:
        # Only the comment line is touched; the removed write is the edge.
        reached = self.reach({8}, removed=["set(_arm_only TRUE)"])
        self.assertEqual(reached.tests, {"arm-control"})

    def test_an_edited_block_header_reaches_the_block(self) -> None:
        self.assertIn("arm-control", self.reach({11}).tests)

    def test_control_an_edited_comment_reaches_nothing(self) -> None:
        reached = self.reach({8})
        self.assertEqual((reached.tests, reached.targets), (set(), set()))

    def test_control_a_parenthesis_in_a_quoted_argument_does_not_end_the_call(self) -> None:
        text = 'add_test(NAME q COMMAND x ")"\n    --flag)\nadd_test(NAME r COMMAND y)\n'
        self.assertEqual(impact.impact(text, {2}).tests, {"q"})


class ChangedLinesTests(unittest.TestCase):
    def test_hunks_give_head_lines_and_removed_text(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True,  # noqa: E731
                                            capture_output=True, text=True).stdout.strip()
            git("init", "-q")
            (repo / "t.cmake").write_text("a\nb\nc\nd\n")
            git("add", "t.cmake")
            git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "base")
            base = git("rev-parse", "HEAD")
            (repo / "t.cmake").write_text("a\nB\nd\n")
            git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qam", "head")
            touched, removed = step.changed_lines(base, "HEAD", ["t.cmake"], repo)["t.cmake"]
        self.assertEqual((touched, removed), ({2}, ["b", "c"]))


class DirectEdgeTests(unittest.TestCase):
    def test_declared_script_inputs_match_files_and_directories(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "test").mkdir()
            (root / step.SCRIPT_INPUTS_LIST).write_text(
                '{"tests": {"census": {"inputs": ["docs/status/", "x.json"]},'
                ' "other": {"inputs": ["y.json"]}}}')
            self.assertEqual(step.script_input_tests(["docs/status/a.yaml"], root), ["census"])
            self.assertEqual(step.script_input_tests(["docs/statusx"], root), [])

    def model(self) -> step.inventory.CodeModel:
        T = step.inventory.Target
        return step.inventory.CodeModel(
            targets={"pulp-probe": T("pulp-probe", "EXECUTABLE", "", "", ["test/probe.cpp"],
                                     artifacts=["bin/pulp-probe"]),
                     "lib": T("lib", "STATIC_LIBRARY", "", "", ["core/lib.cpp"],
                              artifacts=["lib/liblib.a"])},
            source_root="/s", build_root="/b")

    def entries(self) -> list:
        E = step.inventory.CTestEntry
        return [E("probe-run", ["bin/pulp-probe"], {}), E("other", ["bin/x"], {})]

    def patched(self):
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.object(step.inventory, "codemodel_reply_available",
                                              return_value=True))
        stack.enter_context(mock.patch.object(step.inventory, "load_codemodel_targets",
                                              return_value=self.model()))
        stack.enter_context(mock.patch.object(step.inventory, "load_projection_ctest_entries",
                                              return_value=self.entries()))
        return stack

    def test_an_edited_program_source_selects_the_tests_that_run_it(self) -> None:
        with self.patched():
            self.assertEqual(step.own_tests(Path("/b"), REPO, ["test/probe.cpp"]), ["probe-run"])
            self.assertEqual(step.own_tests(Path("/b"), REPO, ["core/lib.cpp"]), [])

    def test_a_registration_edit_reaches_the_tests_that_run_its_target(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "test" / "cmake").mkdir(parents=True)
            (root / "test" / "cmake" / "m.cmake").write_text(MANIFEST)
            with self.patched():
                reached = step.registration_tests(Path("/b"), ["test/cmake/m.cmake"], root,
                                                  {"test/cmake/m.cmake": ({3}, [])})
                untouched = step.registration_tests(Path("/b"), ["test/cmake/m.cmake"], root,
                                                    {"test/cmake/m.cmake": ({8}, [])})
        self.assertIn("probe-run", reached)
        self.assertEqual(untouched, [])

    def test_editing_a_lane_entry_script_reaches_the_lane_contract(self) -> None:
        names = ["source-selftest-lane-contract", "unit-a"]
        # Both entry shapes: a `{repo}/x.py` script and a `-m unittest` module.
        for entry in ("tools/ci/test_pr_head_affected_tests.py",
                      "tools/scripts/test_build_time_report.py"):
            self.assertEqual(step.source_lane_contract_tests([entry], names, REPO),
                             ["source-selftest-lane-contract"], entry)
        self.assertEqual(step.source_lane_contract_tests(["docs/a.md"], names, REPO), [])

    def test_an_unowned_file_is_set_aside_and_the_rest_projected(self) -> None:
        calls: list[list[str]] = []

        def project_affected(model, changed, *rest):
            calls.append(list(changed))
            if "core/new.cpp" in changed:
                return selection("all", [], "source not owned by any configured target: core/new.cpp")
            return selection("focused", ["unit-a"])

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel in ("core/new.cpp", "core/lib.cpp"):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text("")
            with self.patched(), \
                    mock.patch.object(step.affected_targets, "policy_families", return_value=[]), \
                    mock.patch.object(step.inventory, "header_owners", return_value={}), \
                    mock.patch.object(step.inventory, "project_affected",
                                      side_effect=project_affected):
                chosen = step.project(Path("/b"), root, ["core/new.cpp", "core/lib.cpp"])
        self.assertEqual((chosen.mode, chosen.tests), ("focused", ["unit-a"]))
        self.assertEqual(calls[-1], ["core/lib.cpp"])


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
