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
    "sample-region-compat-baseline": set(),
    "no-editor-plugin-link-floor-artifacts": set(),
    "pulp-gpu-host-mapped-pointer-probe": set(), "pulp-gpu-audio-provider-identity": set(),
    "pulp-gpu-dawn-shared-io-provider-probe": set(), "pulp-gpu-compute-unrelated": set(),
    "cmake-ios-auv3-control-shipping": set(),
    "pulp-test-cli-migration-index": set(),
}


class SelectionTests(unittest.TestCase):
    def plan(self, paths: list[str], projected: step.inventory.Selection,
             *, scripts: list[str] = (), inputs: list[str] = (), own: list[str] = (),
             rebuilt: list[str] = (), registered: list[str] = ()) -> dict:
        with mock.patch.object(step, "project", return_value=projected) as project, \
                mock.patch.object(step, "script_reference_tests", return_value=list(scripts)), \
                mock.patch.object(step, "script_input_tests", return_value=list(inputs)), \
                mock.patch.object(step, "own_tests", return_value=list(own)), \
                mock.patch.object(step, "rebuilt_binary_tests", return_value=list(rebuilt)), \
                mock.patch.object(step, "registration_tests", return_value=list(registered)):
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
        self.assertEqual(sorted(plan["tests"]), ["cmake-install-layout",
                                                   "cmake-ios-auv3-control-shipping",
                                                   "cmake-sdk-smoke"])

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

    def test_ios_control_shipping_fixture_reaches_its_cmake_probe(self) -> None:
        for path in ("test/cmake/test_ios_auv3_control_shipping.cmake",
                     "test/cmake/cmake_smoke_tests.cmake"):
            with self.subTest(path=path):
                plan = self.plan([path], selection("all", []))
                self.assertIn("cmake-ios-auv3-control-shipping", plan["tests"])

    def test_no_editor_fixture_reaches_its_artifact_probe(self) -> None:
        plan = self.plan(["test/fixtures/no_editor_link_floor/scan_no_editor_link_floor.cmake"],
                         selection("all", []))
        self.assertIn("no-editor-link-floor", plan["families"])

    def test_p0_2_replay_set_selects_each_merge_group_only_check(self) -> None:
        cases = {
            # #10053(a): the scanner is in the fixture tree, so the family rule
            # reaches the platform link-floor artifact probe.
            "10053a": (["test/fixtures/no_editor_link_floor/scan_no_editor_link_floor.cmake"],
                        "no-editor-plugin-link-floor-artifacts"),
            # #8926 and #9015: the wide tier walks every tools/scripts file;
            # its directory declaration is generated into the input manifest.
            "8926": (["tools/scripts/test_sdk_development_provenance.py"],
                      "wide-non-native-selftest"),
            "9015": (["tools/scripts/gate_python_imports_check.py"],
                      "wide-non-native-selftest"),
            # #9177: the compatibility baseline declares its fixture directory.
            "9177": (["test/fixtures/sample-region-compat/expected/abi.txt"],
                      "sample-region-compat-baseline"),
        }
        for case, (paths, expected) in cases.items():
            with self.subTest(case=case):
                declared = step.script_input_tests(paths, REPO)
                families = step.family_tests(paths, sorted(NAMES))
                selected = set(declared)
                selected.update(name for names in families.values() for name in names)
                self.assertIn(expected, selected,
                              f"replay {case} did not select {expected}: {selected}")

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

    def test_an_unrelated_tools_change_defers_to_the_merge_queue(self) -> None:
        plan = self.plan(["tools/scripts/unknown_reader.py"], selection("focused", []))
        self.assertEqual(plan["tests"], [])
        self.assertTrue(plan["selection_unknown"])

    def test_sample_region_include_closure_paths_select_the_baseline(self) -> None:
        for path in ("core/host/include/pulp/host/custom_node_type.hpp",
                     "core/host/include/pulp/host/signal_graph.hpp",
                     "test/sample_region_compat/compat_runner.cpp"):
            with self.subTest(path=path):
                plan = self.plan([path], selection("focused", []),
                                 inputs=["sample-region-compat-baseline"])
                self.assertIn("sample-region-compat-baseline", plan["tests"])


    def test_merge_only_checks_run_before_ordinary_affected_tests(self) -> None:
        # The projection's own test and a rebuilt binary come first in selection
        # order; the declared-input and path-family checks must still lead the
        # run list, so a budget that fits only them still runs them.
        plan = self.plan(["tools/cmake/PulpPlugin.cmake", "core/x/a.cpp"],
                         selection("focused", ["unit-a"]), rebuilt=["unit-b"],
                         inputs=["sample-region-compat-baseline"])
        merge_only = ["sample-region-compat-baseline", "cmake-install-layout",
                      "cmake-ios-auv3-control-shipping", "cmake-sdk-smoke"]
        self.assertEqual(sorted(plan["merge_only"]), sorted(merge_only))
        self.assertEqual(sorted(plan["tests"][:len(merge_only)]), sorted(merge_only))
        self.assertEqual(sorted(plan["tests"][len(merge_only):]), ["unit-a", "unit-b"])

    def test_a_registration_the_diff_adds_is_a_merge_only_check(self) -> None:
        # A brand-new test has no declared inputs yet; its registration is the
        # only edge that reaches it.
        plan = self.plan(["core/x/a.cpp"], selection("focused", ["unit-a"]),
                         registered=["other"])
        self.assertEqual(plan["merge_only"], ["other"])
        self.assertEqual(plan["tests"][0], "other")

    def test_the_diffs_own_tests_run_right_after_the_merge_only_checks(self) -> None:
        # A rebuilt binary is collected before the own tests; the plan moves the
        # tests built from the edited sources ahead of it.
        plan = self.plan(["core/x/a.cpp", "test/test_a.cpp"],
                         selection("all", [], "too wide"), rebuilt=["unit-b"],
                         own=["unit-a"], inputs=["sample-region-compat-baseline"])
        self.assertEqual(plan["tests"], ["sample-region-compat-baseline", "unit-a", "unit-b"])

    def test_control_a_plan_without_merge_only_checks_keeps_its_order(self) -> None:
        plan = self.plan(["core/x/a.cpp"], selection("focused", ["unit-a"]), rebuilt=["unit-b"])
        self.assertEqual(plan["merge_only"], [])
        self.assertEqual(plan["tests"], ["unit-b", "unit-a"])

    def test_budget_skips_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            with mock.patch.object(step.time, "monotonic", side_effect=[0.0, 2.0, 2.0]):
                result = step.run_budgeted(Path("/build"), ["unit-a"], 1.0, 1, "", root, 1)
        self.assertEqual(result["skipped"], ["unit-a"])

    def test_rebuilt_binary_tests_are_added_even_without_source_projection(self) -> None:
        plan = self.plan(["core/other.cpp"], selection("focused", []),
                         rebuilt=["pulp-test-cli-migration-index"])
        self.assertEqual(plan["tests"], ["pulp-test-cli-migration-index"])

    def test_docs_only_diff_does_not_add_warm_build_rebuilt_tests(self) -> None:
        plan = self.plan(["docs/a.md"], selection("focused", []),
                         rebuilt=["pulp-test-cli-migration-index"])
        self.assertEqual(plan["tests"], [])


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

    def test_rebuilt_binary_output_reaches_its_registered_test(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            build.mkdir()
            (build / ".ninja_log").write_text(
                "# ninja log v5\n0\t12\t0\tbin/pulp-test-cli-migration-index\t1\n",
                encoding="utf-8")
            (build / ".pulp-build-start").write_text("start\n", encoding="utf-8")
            output = build / "bin/pulp-test-cli-migration-index"
            output.parent.mkdir()
            output.write_text("binary\n", encoding="utf-8")
            model = step.inventory.CodeModel(
                targets={"pulp-test-cli-migration-index": step.inventory.Target(
                    "pulp-test-cli-migration-index", "EXECUTABLE", "", "",
                    ["test/migration.cpp"], artifacts=[str(build / "bin/pulp-test-cli-migration-index")])},
                source_root=str(REPO), build_root=str(build))
            entries = [step.inventory.CTestEntry(
                "migration-round-trip", [str(build / "bin/pulp-test-cli-migration-index"), "--check"], {})]
            with mock.patch.object(step.inventory, "codemodel_reply_available", return_value=True), \
                    mock.patch.object(step.inventory, "load_codemodel_targets", return_value=model), \
                    mock.patch.object(step.inventory, "load_projection_ctest_entries", return_value=entries):
                    self.assertEqual(step.rebuilt_binary_tests(build), ["migration-round-trip"])

    def test_rebuilt_binary_from_a_previous_warm_build_is_not_claimed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            (build / "bin").mkdir(parents=True)
            (build / ".ninja_log").write_text(
                "# ninja log v5\n0\t12\t0\tbin/pulp-test-cli-migration-index\t1\n",
                encoding="utf-8")
            output = build / "bin/pulp-test-cli-migration-index"
            output.write_text("old\n", encoding="utf-8")
            marker = build / ".pulp-build-start"
            marker.write_text("start\n", encoding="utf-8")
            os.utime(marker, (output.stat().st_mtime + 10, output.stat().st_mtime + 10))
            with mock.patch.object(step.inventory, "codemodel_reply_available", return_value=True):
                self.assertEqual(step.rebuilt_binary_tests(build), [])

    def _rebuilt_fixture(self, build: Path, count: int):
        """A build whose last Ninja invocation rebuilt ``count`` test executables."""
        (build / "bin").mkdir(parents=True)
        marker = build / ".pulp-build-start"
        marker.write_text("start\n", encoding="utf-8")
        before = marker.stat().st_mtime - 100
        os.utime(marker, (before, before))
        names = [f"pulp-test-{i}" for i in range(count)]
        log = "# ninja log v5\n" + "".join(
            f"0\t{i + 1}\t0\tbin/{name}\t{i}\n" for i, name in enumerate(names))
        (build / ".ninja_log").write_text(log, encoding="utf-8")
        T = step.inventory.Target
        targets = {}
        entries = []
        for name in names:
            out = build / "bin" / name
            out.write_text("binary\n", encoding="utf-8")
            targets[name] = T(name, "EXECUTABLE", "", "", [f"test/{name}.cpp"],
                              artifacts=[str(out)])
            entries.append(step.inventory.CTestEntry(f"{name}-run", [str(out)], {}))
        model = step.inventory.CodeModel(targets=targets, source_root=str(REPO),
                                         build_root=str(build))
        return names, mock.patch.multiple(
            step.inventory,
            codemodel_reply_available=mock.DEFAULT, load_codemodel_targets=mock.DEFAULT,
            load_projection_ctest_entries=mock.DEFAULT), model, entries

    def _rebuilt(self, count: int, *, regenerate: str | None = None) -> tuple[list[str], list[str]]:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            _, patcher, model, entries = self._rebuilt_fixture(build, count)
            if regenerate:
                (build / regenerate).write_text("regenerated\n", encoding="utf-8")
            why: list[str] = []
            with patcher as mocks:
                mocks["codemodel_reply_available"].return_value = True
                mocks["load_codemodel_targets"].return_value = model
                mocks["load_projection_ctest_entries"].return_value = entries
                return step.rebuilt_binary_tests(build, why), why

    def test_a_narrow_rebuilt_set_is_selected(self) -> None:
        # Control for the cap: up to REBUILT_EXECUTABLE_CAP rebuilt
        # executables still reach their tests.
        tests, why = self._rebuilt(step.REBUILT_EXECUTABLE_CAP)
        self.assertEqual(len(tests), step.REBUILT_EXECUTABLE_CAP)
        self.assertEqual(why, [])

    def test_a_directory_wide_rebuild_adds_nothing(self) -> None:
        # A cold, reconfigured or stale warm directory rebuilds every test
        # executable; the diff reached none of that, so none is selected.
        tests, why = self._rebuilt(40)
        self.assertEqual(tests, [])
        self.assertIn("40 rebuilt executable(s) ignored", why[0])

    def test_a_regenerated_build_system_adds_nothing(self) -> None:
        for name in ("build.ninja", "CMakeCache.txt"):
            with self.subTest(regenerated=name):
                tests, why = self._rebuilt(2, regenerate=name)
                self.assertEqual(tests, [])
                self.assertIn(f"{name} was regenerated", why[0])

    def test_select_keeps_only_the_projection_when_the_rebuild_is_wide(self) -> None:
        # End to end through select(): the last invocation rebuilt every
        # executable, the diff projects onto one test, and only that runs.
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            names, patcher, model, entries = self._rebuilt_fixture(build, 40)
            all_names = {**NAMES, **{e.name: set() for e in entries}}
            with patcher as mocks, \
                    mock.patch.object(step, "project", return_value=selection("focused", ["unit-a"])), \
                    mock.patch.object(step, "script_reference_tests", return_value=[]), \
                    mock.patch.object(step, "script_input_tests", return_value=[]), \
                    mock.patch.object(step, "registration_tests", return_value=[]):
                mocks["codemodel_reply_available"].return_value = True
                mocks["load_codemodel_targets"].return_value = model
                mocks["load_projection_ctest_entries"].return_value = entries
                plan = step.select(build, ["core/a.cpp"], all_names)
        self.assertEqual(plan["tests"], ["unit-a"])
        self.assertEqual(plan["rebuilt_binaries"], 0)
        self.assertIn("ignored", plan["rebuilt_binaries_ignored"])

    def test_rebuilt_binary_without_a_ninja_log_is_not_claimed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            build.mkdir()
            with mock.patch.object(step.inventory, "codemodel_reply_available", return_value=True):
                self.assertEqual(step.rebuilt_binary_tests(build), [])

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
        # which a test named `bad-*` fails, `skip-*` self-skips, and the rest pass.
        (self.bin / "ctest").write_text(
            "#!/usr/bin/env python3\n"
            "import sys, time\n"
            "a = sys.argv\n"
            "names = open(a[a.index('--tests-from-file') + 1]).read().split()\n"
            "time.sleep(float(__import__('os').environ.get('FAKE_CTEST_SLEEP', '0')))\n"
            "cases = ''.join(f'<testcase name=\"{n}\" status=\"{\"fail\" if n.startswith(\"bad\") else \"notrun\" if n.startswith(\"skip\") else \"run\"}\">'\n"
            "                + ('<failure/>' if n.startswith('bad') else '<skipped/>' if n.startswith('skip') else '') + '</testcase>' for n in names)\n"
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
        self.assertEqual(results["failure_evidence"]["bad-b"],
                         "ctest reported a failure without detail")
        self.assertEqual(sorted(results["passed"]), ["a", "c"])
        self.assertEqual(results["skipped"], [])

    def test_failure_evidence_uses_the_first_failing_line_from_real_junit(self) -> None:
        junit = self.tmp / "evidence.xml"
        junit.write_text('<testsuite><testcase name="bad"><failure>assert x == y\nsecond line</failure></testcase></testsuite>',
                         encoding="utf-8")
        self.assertEqual(step.junit_failure_evidence(junit), {"bad": "assert x == y"})

    def test_annotation_evidence_is_safe_for_one_line_workflow_commands(self) -> None:
        self.assertEqual(step.annotation_escape("100%\nnext"), "100%25%0Anext")

    def test_no_batch_starts_after_the_budget(self) -> None:
        with mock.patch.dict(os.environ, {"FAKE_CTEST_SLEEP": "0.3"}):
            results = step.run_budgeted(self.tmp, ["a", "b", "c", "d"], 0.2, 1, "", self.tmp, 2)
        self.assertEqual(sorted(results["passed"]), ["a", "b"])
        self.assertEqual(results["skipped"], ["c", "d"])

    def test_a_budget_that_fits_only_the_merge_only_checks_still_runs_them(self) -> None:
        # Path-family checks are collected after the projection's tests; the
        # plan moves them ahead, so one batch is enough to run them.
        with mock.patch.object(step, "project", return_value=selection("focused", ["unit-a", "unit-b"])), \
                mock.patch.object(step, "script_reference_tests", return_value=[]), \
                mock.patch.object(step, "script_input_tests", return_value=[]), \
                mock.patch.object(step, "own_tests", return_value=[]), \
                mock.patch.object(step, "rebuilt_binary_tests", return_value=[]), \
                mock.patch.object(step, "registration_tests", return_value=[]):
            plan = step.select(Path("/b"), ["tools/cmake/PulpPlugin.cmake", "core/x/a.cpp"], NAMES)
        family = ["cmake-install-layout", "cmake-ios-auv3-control-shipping", "cmake-sdk-smoke"]
        with mock.patch.dict(os.environ, {"FAKE_CTEST_SLEEP": "0.3"}):
            results = step.run_budgeted(self.tmp, plan["tests"], 0.2, 1, "", self.tmp, len(family))
        self.assertEqual(sorted(results["passed"]), family)
        self.assertEqual(results["skipped"], ["unit-a", "unit-b"])

    def test_ctest_self_skips_are_reported_notrun_not_budget_skipped(self) -> None:
        results = step.run_budgeted(self.tmp, ["skip-self"], 60, 1, "", self.tmp, 1)
        self.assertEqual(results["notrun"], ["skip-self"])
        self.assertEqual(results["skipped"], [])


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
            {"name": "script-diff-scoped",
             "command": ["/usr/bin/python3", str(REPO / "tools/scripts/d.py"), "--check"],
             "properties": [{"name": "PULP_DIFF_SCOPED", "value": "TRUE"}]},
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
        # A diff-scoped check sees an empty diff on the base checkout, so its
        # base re-run passes whatever the base's state.
        self.lane.rerun_on_base.side_effect = lambda still, entries, ref: {
            r["name"]: ((False, "passes on the base") if r["name"] == "script-diff-scoped"
                        else (r["name"] == "script-red", "why")) for r in still}
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


    def test_a_diff_scoped_check_is_not_judged_by_its_base_rerun(self) -> None:
        verdicts = step.label_base_failures(
            self.build, ["script-red", "script-diff-scoped"], "HEAD^1")
        self.assertEqual(verdicts["script-diff-scoped"], (False, step.NOT_COMPARABLE))
        self.assertIn("not comparable", verdicts["script-diff-scoped"][1])
        self.assertNotIn("passes on the base", verdicts["script-diff-scoped"][1])
        # Neither re-run here nor on the base: no run can speak for it.
        ran = [e["name"] for call in self.lane.run.call_args_list for e in call[0][0]]
        self.assertEqual(ran, ["script-red"])
        still = self.lane.rerun_on_base.call_args[0][0]
        self.assertEqual([r["name"] for r in still], ["script-red"])
        self.assertEqual(verdicts["script-red"][0], True)

    def test_control_without_the_property_the_base_rerun_decides(self) -> None:
        inventory = __import__("json").loads((self.tmp / "inv.json").read_text(encoding="utf-8"))
        for test in inventory["tests"]:
            test.pop("properties", None)
        (self.tmp / "inv.json").write_text(__import__("json").dumps(inventory), encoding="utf-8")
        verdicts = step.label_base_failures(self.build, ["script-diff-scoped"], "HEAD^1")
        self.assertEqual(verdicts["script-diff-scoped"], (False, "passes on the base"))


class DiffScopedRegistrationTests(unittest.TestCase):
    @staticmethod
    def registration(name: str, marked: bool | None) -> dict:
        props = [] if marked is None else [{"name": "PULP_DIFF_SCOPED",
                                             "value": "TRUE" if marked else "FALSE"}]
        return {"name": name, "properties": props}

    def test_both_marked_is_clean(self) -> None:
        tests = [self.registration(n, True) for n in step.DIFF_SCOPED_TESTS]
        self.assertEqual(step.diff_scoped_registration_problems(tests), [])

    def test_an_unmarked_registration_is_named(self) -> None:
        for missing in (None, False):
            with self.subTest(missing=missing):
                tests = [self.registration("script-test-inputs-drift", True),
                         self.registration("changed-surface-script-families-drift", missing)]
                problems = step.diff_scoped_registration_problems(tests)
                self.assertEqual(len(problems), 1)
                self.assertIn("changed-surface-script-families-drift", problems[0])

    def test_a_check_this_configure_does_not_register_is_not_judged(self) -> None:
        tests = [self.registration("script-test-inputs-drift", True)]
        self.assertEqual(step.diff_scoped_registration_problems(tests), [])

    def test_control_an_inventory_without_the_anchor_check_fails(self) -> None:
        problems = step.diff_scoped_registration_problems([self.registration("other", None)])
        self.assertEqual(len(problems), 1)
        self.assertIn("wrong build directory", problems[0])


class ExitCodeTests(unittest.TestCase):
    """A red main does not fail the pull request; its own failure does."""

    def _main(self, verdicts: dict, *, skipped: list[str] | None = None,
              notrun: list[str] | None = None, merge_only: list[str] | None = None) -> int:
        plan = {"changed": 1, "mode": "focused", "reason": "r", "families": {},
                "tests": ["a", "b", "c"], "merge_only": merge_only or []}
        results = {"passed": [], "failed": list(verdicts), "skipped": skipped or [],
                   "notrun": notrun or [],
                   "excluded_or_absent": [], "seconds": 1.0}
        out = io.StringIO()
        with mock.patch.object(step, "diff_paths", return_value=["x.py"]), \
                mock.patch.object(step, "inventory_tests", return_value={}), \
                mock.patch.object(step, "select", return_value=plan), \
                mock.patch.object(step, "run_budgeted", return_value=results), \
                mock.patch.object(step, "label_base_failures", return_value=verdicts), \
                contextlib.redirect_stdout(out):
            code = step.main(["--build-dir", "/nonexistent"])
        self.output = out.getvalue()
        return code

    def test_every_failure_pre_existing_passes(self) -> None:
        self.assertEqual(self._main({"a": (True, "same on base")}), 0)

    def test_control_one_branch_caused_failure_fails(self) -> None:
        self.assertEqual(self._main({"a": (True, "same"), "b": (False, "passes on base")}), 1)

    def test_budget_skipped_selection_fails_the_real_step(self) -> None:
        # A merge-only check the budget pushed out has run nowhere yet.
        self.assertEqual(self._main({}, skipped=["b"], merge_only=["b"]), 1)
        self.assertIn("SKIPPED, NOT RUN", self.output)
        self.assertNotIn("the merge group runs them", self.output)

    def test_ordinary_budget_skips_defer_to_the_merge_queue(self) -> None:
        self.assertEqual(self._main({}, skipped=["b", "c"], merge_only=["a"]), 0)
        self.assertIn("::warning title=pr-head-affected-tests::2 affected test(s) deferred to "
                      "merge queue (budget spent): b, c", self.output)
        self.assertIn('"deferred_to_merge_queue": 2', self.output)
        self.assertIn('"skipped_for_budget": 2', self.output)

    def test_a_merge_only_skip_fails_even_beside_ordinary_deferrals(self) -> None:
        self.assertEqual(self._main({}, skipped=["b", "c"], merge_only=["b"]), 1)
        self.assertIn('"merge_only_skipped_for_budget": 1', self.output)

    def test_a_newly_registered_test_skipped_for_budget_fails_the_real_step(self) -> None:
        # The plan comes from the real select(): the diff registers "other",
        # which has no declared inputs, and the budget pushes it out.
        with mock.patch.object(step, "project", return_value=selection("focused", ["unit-a"])), \
                mock.patch.object(step, "script_reference_tests", return_value=[]), \
                mock.patch.object(step, "script_input_tests", return_value=[]), \
                mock.patch.object(step, "own_tests", return_value=[]), \
                mock.patch.object(step, "rebuilt_binary_tests", return_value=[]), \
                mock.patch.object(step, "registration_tests", return_value=["other"]):
            plan = step.select(Path("/b"), ["core/x/a.cpp"], NAMES)
        results = {"passed": ["unit-a"], "failed": [], "skipped": ["other"], "notrun": [],
                   "excluded_or_absent": [], "seconds": 1.0}
        out = io.StringIO()
        with mock.patch.object(step, "diff_paths", return_value=["x.py"]), \
                mock.patch.object(step, "inventory_tests", return_value={}), \
                mock.patch.object(step, "select", return_value=plan), \
                mock.patch.object(step, "run_budgeted", return_value=results), \
                contextlib.redirect_stdout(out):
            self.assertEqual(step.main(["--build-dir", "/nonexistent"]), 1)
        self.assertIn("SKIPPED, NOT RUN", out.getvalue())

    def test_control_a_real_failure_still_fails_beside_ordinary_deferrals(self) -> None:
        self.assertEqual(self._main({"a": (False, "passes on base")}, skipped=["c"]), 1)

    def test_ctest_self_skip_does_not_fail_the_real_step(self) -> None:
        self.assertEqual(self._main({}, notrun=["b"]), 0)

    def test_unknown_selection_defers_neutrally_to_the_merge_queue(self) -> None:
        plan = {"changed": 1, "mode": "focused", "reason": "r", "families": {},
                "tests": [], "selection_unknown": True}
        with mock.patch.object(step, "diff_paths", return_value=["tools/scripts/new.py"]), \
                mock.patch.object(step, "inventory_tests", return_value={}), \
                mock.patch.object(step, "select", return_value=plan), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(step.main(["--build-dir", "/nonexistent"]), 0)

    def test_unknown_selection_keeps_known_tests(self) -> None:
        plan = {"changed": 1, "mode": "focused", "reason": "r", "families": {},
                "tests": ["known"], "selection_unknown": True}
        results = {"passed": ["known"], "failed": [], "skipped": [], "notrun": [],
                   "excluded_or_absent": [], "seconds": 0.1}
        with mock.patch.object(step, "diff_paths", return_value=["tools/scripts/new.py"]), \
                mock.patch.object(step, "inventory_tests", return_value={}), \
                mock.patch.object(step, "select", return_value=plan), \
                mock.patch.object(step, "run_budgeted", return_value=results) as run, \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(step.main(["--build-dir", "/nonexistent"]), 0)
        run.assert_called_once()


if __name__ == "__main__":
    unittest.main()
