#!/usr/bin/env python3
"""Tests for wide_non_native.py and classify_changes.py --wide-non-native.

A wrong admission lets a tooling change skip the macOS gate while a test on
that gate could have caught it, so most cases here are fail-closed ones. The
reference-graph cases run against a throwaway git repository; the contract
cases read the live tree; the replay runs the classifier over the recorded
changed-file lists of 177 merged pull requests.

Run:
    python3 tools/scripts/test_wide_non_native.py
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

THIS_DIR = Path(__file__).resolve().parent
REPO = THIS_DIR.parents[1]


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, THIS_DIR / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


wide = _load("wide_non_native")
classify = _load("classify_changes")

LANE_TEST = "tools/scripts/test_lane_thing.py"
TIER_SCRIPT = "tools/scripts/tree_scanner.py"


class FixtureRepo:
    """A minimal git repository with a lane manifest and a tier manifest."""

    def __init__(self, files: dict[str, str]) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        base = {
            wide.LANE_MANIFEST: json.dumps(
                {
                    "schema_version": 1,
                    "tests": [
                        {"name": "lane-thing-selftest", "argv": ["{repo}/" + LANE_TEST]}
                    ],
                }
            ),
            wide.TIER_MANIFEST: json.dumps(
                {
                    "schema_version": 1,
                    "tests": [{"name": "tree-scanner", "argv": ["{repo}/" + TIER_SCRIPT]}],
                }
            ),
            LANE_TEST: "import lane_thing\n",
            TIER_SCRIPT: "# walks tools/\n",
        }
        base.update(files)
        for rel, text in base.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        for args in (
            ["init", "-q"],
            ["add", "-A"],
            ["-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "fixture"],
        ):
            subprocess.run(["git", *args], cwd=self.root, check=True)

    def decide(self, *files: str) -> dict:
        return wide.classify(list(files), repo=self.root)

    def close(self) -> None:
        self._tmp.cleanup()


class ReferenceGraphTests(unittest.TestCase):
    def _repo(self, files: dict[str, str]) -> FixtureRepo:
        repo = FixtureRepo(files)
        self.addCleanup(repo.close)
        return repo

    def test_script_named_only_by_its_lane_test_is_admitted(self) -> None:
        repo = self._repo({"tools/scripts/lane_thing.py": "X = 1\n"})
        d = repo.decide("tools/scripts/lane_thing.py", LANE_TEST)
        self.assertTrue(all(v.admitted for v in d.values()), d)

    def test_lane_test_outside_the_candidate_trees_is_accepted(self) -> None:
        # A lane test under tools/ci is not itself a candidate, so only its
        # lane membership can vouch for the script it names.
        repo = self._repo({
            "tools/scripts/ci_thing.py": "X = 1\n",
            "tools/ci/test_ci_thing.py": "import ci_thing\n",
            wide.LANE_MANIFEST: json.dumps({"schema_version": 1, "tests": [
                {"name": "ci-thing-selftest", "argv": ["{repo}/tools/ci/test_ci_thing.py"]},
            ]}),
        })
        self.assertTrue(repo.decide("tools/scripts/ci_thing.py")[
            "tools/scripts/ci_thing.py"].admitted)

    def test_unreferenced_script_is_admitted(self) -> None:
        repo = self._repo({"tools/scripts/orphan_tool.py": "X = 1\n"})
        self.assertTrue(repo.decide("tools/scripts/orphan_tool.py")[
            "tools/scripts/orphan_tool.py"].admitted)

    def test_lane_registration_in_cmake_is_accepted(self) -> None:
        repo = self._repo({
            "tools/scripts/lane_thing.py": "X = 1\n",
            "test/cmake/quality_tests.cmake": (
                "add_test(NAME lane-thing-selftest COMMAND python3\n"
                '    "${CMAKE_SOURCE_DIR}/tools/scripts/lane_thing.py")\n'
            ),
        })
        self.assertTrue(repo.decide("tools/scripts/lane_thing.py")[
            "tools/scripts/lane_thing.py"].admitted)

    def test_gate_side_cmake_registration_keeps_native(self) -> None:
        repo = self._repo({
            "tools/scripts/gate_thing.py": "X = 1\n",
            "test/cmake/quality_tests.cmake": (
                "add_test(NAME gate-thing COMMAND python3\n"
                '    "${CMAKE_SOURCE_DIR}/tools/scripts/gate_thing.py")\n'
            ),
        })
        d = repo.decide("tools/scripts/gate_thing.py")["tools/scripts/gate_thing.py"]
        self.assertFalse(d.admitted)
        self.assertIn("test/cmake/quality_tests.cmake", d.reason)

    def test_cmake_mention_outside_add_test_keeps_native(self) -> None:
        repo = self._repo({
            "tools/scripts/lane_thing.py": "X = 1\n",
            "CMakeLists.txt": 'file(READ "tools/scripts/lane_thing.py" _x)\n',
        })
        self.assertFalse(repo.decide("tools/scripts/lane_thing.py")[
            "tools/scripts/lane_thing.py"].admitted)

    def test_helper_of_lane_test_is_admitted_transitively(self) -> None:
        repo = self._repo({
            "tools/scripts/lane_thing.py": "import deep_helper\n",
            "tools/scripts/deep_helper.py": "X = 1\n",
        })
        self.assertTrue(repo.decide("tools/scripts/deep_helper.py")[
            "tools/scripts/deep_helper.py"].admitted)

    def test_helper_beyond_the_searched_depth_keeps_native(self) -> None:
        # deep_helper is named only by mid_helper, which is named only by the
        # lane test. With one level of search mid_helper's own referrers are
        # never read, so it cannot vouch for deep_helper.
        repo = self._repo({
            "tools/scripts/lane_thing.py": "import mid_helper\n",
            "tools/scripts/mid_helper.py": "import deep_helper\n",
            "tools/scripts/deep_helper.py": "X = 1\n",
        })
        self.assertTrue(repo.decide("tools/scripts/deep_helper.py")[
            "tools/scripts/deep_helper.py"].admitted)
        with mock.patch.object(wide, "MAX_DEPTH", 1):
            d = repo.decide("tools/scripts/deep_helper.py")
        self.assertFalse(d["tools/scripts/deep_helper.py"].admitted)

    def test_helper_of_gate_registered_script_keeps_native(self) -> None:
        repo = self._repo({
            "tools/scripts/gate_thing.py": "import deep_helper\n",
            "tools/scripts/deep_helper.py": "X = 1\n",
            "test/cmake/quality_tests.cmake": (
                "add_test(NAME gate-thing COMMAND python3\n"
                '    "${CMAKE_SOURCE_DIR}/tools/scripts/gate_thing.py")\n'
            ),
        })
        d = repo.decide("tools/scripts/deep_helper.py")["tools/scripts/deep_helper.py"]
        self.assertFalse(d.admitted)
        self.assertIn("gate_thing.py", d.reason)

    def test_gate_workflow_reference_keeps_native(self) -> None:
        # Literal paths, not wide.GATE_WORKFLOWS: emptying that constant must
        # fail here rather than silently shrink the loop to nothing.
        for workflow in (".github/workflows/build.yml",
                         ".github/workflows/build-macos.yml"):
            with self.subTest(workflow=workflow):
                repo = self._repo({
                    "tools/scripts/route_thing.py": "X = 1\n",
                    workflow: "run: python3 tools/scripts/route_thing.py\n",
                })
                self.assertFalse(repo.decide("tools/scripts/route_thing.py")[
                    "tools/scripts/route_thing.py"].admitted)

    def test_composite_action_reference_keeps_native(self) -> None:
        repo = self._repo({
            "tools/scripts/route_thing.py": "X = 1\n",
            ".github/actions/setup/action.yml": "run: tools/scripts/route_thing.py\n",
        })
        self.assertFalse(repo.decide("tools/scripts/route_thing.py")[
            "tools/scripts/route_thing.py"].admitted)

    def test_other_workflow_reference_is_inert(self) -> None:
        repo = self._repo({
            "tools/scripts/watch_thing.py": "X = 1\n",
            ".github/workflows/nightly.yml": "run: tools/scripts/watch_thing.py\n",
        })
        self.assertTrue(repo.decide("tools/scripts/watch_thing.py")[
            "tools/scripts/watch_thing.py"].admitted)

    def test_native_referrers_keep_native(self) -> None:
        for referrer in (
            "tools/ci/governed_wrapper.sh",
            "tools/cmake/PulpThing.cmake",
            "test/test_thing.cpp",
            "core/view/src/thing.cpp",
            "tools/cli/cmd_thing.cpp",
            "examples/demo/run.sh",
        ):
            with self.subTest(referrer=referrer):
                repo = self._repo({
                    "tools/scripts/used_thing.py": "X = 1\n",
                    referrer: "used_thing\n",
                })
                self.assertFalse(repo.decide("tools/scripts/used_thing.py")[
                    "tools/scripts/used_thing.py"].admitted)

    def test_paths_outside_the_widened_trees_keep_native(self) -> None:
        repo = self._repo({})
        for path in (
            "core/view/src/widgets.cpp",
            "CMakeLists.txt",
            "test/cmake/quality_tests.cmake",
            "tools/cmake/PulpTestSuite.cmake",
            "tools/ci/governed-build.sh",
            ".github/workflows/build.yml",
            ".claude-plugin/plugin.json",
            "tools/cli/cmd_pr.cpp",
        ):
            with self.subTest(path=path):
                self.assertFalse(repo.decide(path)[path].admitted)

    def test_hard_native_files_are_never_admitted(self) -> None:
        repo = self._repo({p: "X = 1\n" for p in wide.HARD_NATIVE})
        for path in sorted(wide.HARD_NATIVE):
            with self.subTest(path=path):
                self.assertFalse(repo.decide(path)[path].admitted)

    def test_missing_manifest_keeps_native(self) -> None:
        repo = self._repo({"tools/scripts/orphan_tool.py": "X = 1\n"})
        (repo.root / wide.TIER_MANIFEST).unlink()
        d = repo.decide("tools/scripts/orphan_tool.py")["tools/scripts/orphan_tool.py"]
        self.assertFalse(d.admitted)
        self.assertIn("manifest unreadable", d.reason)

    def test_git_grep_failure_keeps_native(self) -> None:
        repo = self._repo({"tools/scripts/orphan_tool.py": "X = 1\n"})
        failed = subprocess.CompletedProcess([], 128, "", "fatal")
        with mock.patch.object(wide.subprocess, "run", return_value=failed):
            d = repo.decide("tools/scripts/orphan_tool.py")["tools/scripts/orphan_tool.py"]
        self.assertFalse(d.admitted)

    def test_exhausted_search_budget_keeps_native(self) -> None:
        repo = self._repo({"tools/scripts/orphan_tool.py": "X = 1\n"})
        d = wide.classify(["tools/scripts/orphan_tool.py"], repo=repo.root, budget=0)
        self.assertFalse(d["tools/scripts/orphan_tool.py"].admitted)

    def test_mixed_change_keeps_the_native_file_native(self) -> None:
        repo = self._repo({"tools/scripts/orphan_tool.py": "X = 1\n"})
        d = repo.decide("tools/scripts/orphan_tool.py", "core/x.cpp")
        self.assertTrue(d["tools/scripts/orphan_tool.py"].admitted)
        self.assertFalse(d["core/x.cpp"].admitted)


class ClassifierFlagTests(unittest.TestCase):
    def _run(self, *argv: str) -> tuple[dict, str]:
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.dict("os.environ", {"GITHUB_OUTPUT": ""}), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            classify.main(["--mode=files", "--json", *argv])
        return json.loads(out.getvalue()), err.getvalue()

    def test_flag_off_never_consults_the_widening(self) -> None:
        with mock.patch.object(classify, "_wide_decisions",
                               side_effect=AssertionError("consulted")):
            payload, _ = self._run("tools/scripts/some_tool.py")
        self.assertTrue(payload["native_build_required"])

    def test_flag_off_output_is_unchanged_by_the_flag_code(self) -> None:
        # Same input, flag off: identical JSON whether or not the widening
        # would admit the file.
        admit = {"tools/scripts/some_tool.py": wide.Decision(True, "x")}
        with mock.patch.object(classify, "_wide_decisions", return_value=admit):
            with_patch, err_a = self._run("tools/scripts/some_tool.py")
        plain, err_b = self._run("tools/scripts/some_tool.py")
        self.assertEqual(with_patch, plain)
        self.assertEqual(err_a, err_b)
        self.assertEqual(
            set(plain),
            {"native_build_required", "ios_compile_required",
             "agent_capability_installed_sdk_required", "changed_file_count",
             "reason"},
        )

    def test_flag_on_skips_native_when_every_file_is_admitted(self) -> None:
        admit = {"tools/scripts/some_tool.py": wide.Decision(True, "x")}
        with mock.patch.object(classify, "_wide_decisions", return_value=admit):
            payload, _ = self._run("--wide-non-native", "tools/scripts/some_tool.py",
                                   "docs/guides/x.md")
        self.assertFalse(payload["native_build_required"])
        self.assertIn("wide-non-native", payload["reason"])

    def test_flag_on_keeps_native_when_one_file_is_refused(self) -> None:
        decisions = {
            "tools/scripts/some_tool.py": wide.Decision(True, "x"),
            "tools/scripts/other_tool.py": wide.Decision(False, "named by y"),
        }
        with mock.patch.object(classify, "_wide_decisions", return_value=decisions):
            payload, err = self._run("--wide-non-native",
                                     "tools/scripts/some_tool.py",
                                     "tools/scripts/other_tool.py")
        self.assertTrue(payload["native_build_required"])
        self.assertIn("other_tool.py", payload["reason"])
        self.assertNotIn("some_tool.py", payload["reason"])
        self.assertIn("named by y", err)

    def test_flag_on_never_overrides_the_installed_sdk_proof(self) -> None:
        with mock.patch.object(classify, "_wide_decisions",
                               side_effect=AssertionError("consulted")):
            payload, _ = self._run("--wide-non-native",
                                   "tools/scripts/agent_capability_manifest.py")
        self.assertTrue(payload["native_build_required"])

    def test_widening_error_keeps_native(self) -> None:
        with mock.patch.object(wide, "classify", side_effect=RuntimeError("boom")):
            payload, err = self._run("--wide-non-native", "tools/scripts/some_tool.py")
        self.assertTrue(payload["native_build_required"])
        self.assertIn("unavailable", err)

    def test_incomplete_decision_set_keeps_native(self) -> None:
        with mock.patch.object(wide, "classify", return_value={}):
            payload, _ = self._run("--wide-non-native", "tools/scripts/some_tool.py")
        self.assertTrue(payload["native_build_required"])


def _gate_registrations() -> list[tuple[str, str, str]]:
    """(name, add_test body, whole file) for every CMake registration in test/."""
    regs = []
    for path in sorted((REPO / "test").rglob("*.cmake")) + [REPO / "test/CMakeLists.txt"]:
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r"add_test\s*\(\s*NAME\s+([^\s)]+)(.*?)\)", text, re.S):
            regs.append((match.group(1), match.group(2), text))
    return regs


# Gate-side registered scripts that walk a directory tree and mention tools/,
# reviewed as NOT reading tools/scripts, tools/testing or
# tools/import-validation content. A new walker that is neither here nor in the
# tier manifest fails test_every_tools_walking_gate_scanner_is_accounted_for.
REVIEWED_SCANNERS = {
    "tools/scripts/agent_capability_manifest.py":
        "reads core/*/include; its tools/scripts inputs route to the native "
        "build through AGENT_CAPABILITY_INSTALLED_SDK_PATTERNS",
    "tools/scripts/consumption_census.py": "counts installed headers under build roots",
    "tools/scripts/consumption_census_contract.py": "runs consumption_census.py by name",
    "tools/scripts/raw_this_async_check.py": "walks core/ only",
    "tools/scripts/sample_region_compat_baseline.py": "walks CMake and build trees",
    "tools/scripts/skills_doc_check.py": "walks .agents/skills",
    "tools/scripts/style_dedup_table.py": "walks C++ source directories",
    "tools/scripts/thread_assert_check.py": "walks test/*.cpp",
    "tools/scripts/test_agent_capability_installed_sdk.py": "walks its build prefix",
    "tools/scripts/test_build_combined_installer.py": "walks a built bundle",
    "tools/scripts/test_classify_changes.py": "walks CMake the iOS configure reaches",
    "tools/scripts/test_configure_check_cache.py": "walks a temporary cache dir",
    "tools/scripts/test_gpu_first_visible_a3_role_producers.py": "walks a temporary tree",
    "tools/scripts/test_gpu_probe_acceptance.py": "walks temporary directories",
    "tools/scripts/test_gpu_test_resource_locks.py": "walks test/ and tools/cli CMake",
    "tools/scripts/test_sdk_plist_templates_installed.py": "walks tools/cmake",
    "tools/scene3d/verify_renderer_probe_route_inventory_contract.py":
        "walks tools/scene3d",
    "tools/import-validation/check_agent_panel_invariants.py": "walks a build tree",
    "tools/rack/test_generate_safety.py": "walks tools/rack",
    "tools/scripts/doxygen_installed_header_check.py":
        "walks installed header roots (core, tools/cli, tools/audio)",
    "tools/scripts/check_inspector_protocol_registry.py":
        "walks inspect, core, pulp-rs, tools/cli and tools/mcp",
    "tools/scripts/check_skip_not_pass.py": "globs C++ test sources",
    "tools/scripts/msvc_string_literal_guard.py":
        "walks core, inspect, ship, tools/cli, examples, apple",
    "tools/scripts/mac_objc_source_list_guard.py": "globs core/*/platform/mac",
    "tools/scripts/web_timeline_source_closure_check.py": "walks core/*/src",
    "tools/scripts/test_wide_non_native.py":
        "this file; wide_non_native.HARD_NATIVE keeps any change to it native",
}
_WALK = re.compile(r"rglob|os\.walk|ls-files|\.glob\(|\bfind\s+[\"'$.]")
_TOOLS = re.compile(r"""["']tools["'/]|Path\(["']tools|tools/scripts""")


class LiveTreeContractTests(unittest.TestCase):
    def test_tier_entries_mirror_gate_registrations(self) -> None:
        tier = json.loads((REPO / wide.TIER_MANIFEST).read_text(encoding="utf-8"))
        regs = _gate_registrations()
        for entry in tier["tests"]:
            script = entry["argv"][0].replace("{repo}/", "")
            flags = [a for a in entry["argv"][1:] if a.startswith("--")]
            with self.subTest(entry=entry["name"]):
                self.assertTrue((REPO / script).is_file(), script)
                # A flag may arrive through a CMake list variable set in the
                # same manifest, so look for it in that file.
                matching = [
                    body for _, body, text in regs
                    if script in body and all(flag in text for flag in flags)
                ]
                self.assertTrue(matching, f"no gate registration runs {script} {flags}")

    def test_tier_entries_are_portable_to_the_linux_lane(self) -> None:
        sys.path.insert(0, str(REPO / "tools" / "ci"))
        selftests = importlib.import_module("source_selftests")
        entries = selftests.load_manifest(REPO / wide.TIER_MANIFEST)
        for entry in entries:
            errors = [
                e for e in selftests.portability_errors(entry, REPO)
                # The tier step installs PyYAML before it runs.
                if "'import yaml'" not in e
            ]
            self.assertEqual(errors, [], entry["name"])

    def test_tier_step_is_wired_into_the_required_context(self) -> None:
        text = (REPO / ".github/workflows/version-skill-check.yml").read_text()
        self.assertIn("--manifest tools/ci/wide_non_native_checks.json", text)
        self.assertIn("vars.PULP_CLASSIFY_WIDE_NON_NATIVE == '1'", text)
        build = (REPO / ".github/workflows/build.yml").read_text()
        self.assertIn("WIDE_NON_NATIVE: ${{ vars.PULP_CLASSIFY_WIDE_NON_NATIVE }}", build)
        self.assertIn('wide_args=(--wide-non-native)', build)

    def test_every_tools_walking_gate_scanner_is_accounted_for(self) -> None:
        lane = {
            e["argv"][0].replace("{repo}/", "")
            for e in json.loads((REPO / wide.LANE_MANIFEST).read_text())["tests"]
        }
        tier = {
            e["argv"][0].replace("{repo}/", "")
            for e in json.loads((REPO / wide.TIER_MANIFEST).read_text())["tests"]
        }
        unaccounted = []
        seen = set()
        for _, body, _text in _gate_registrations():
            for script in re.findall(r"((?:tools|test)/[\w./-]+\.(?:py|sh))", body):
                if script in seen or script in lane:
                    continue
                seen.add(script)
                path = REPO / script
                if not path.is_file():
                    continue
                text = path.read_text(encoding="utf-8", errors="replace")
                if not (_WALK.search(text) and _TOOLS.search(text)):
                    continue
                if script not in tier and script not in REVIEWED_SCANNERS:
                    unaccounted.append(script)
        self.assertEqual(
            unaccounted, [],
            "gate scanners that walk a tree and mention tools/ must be run by "
            f"the tier ({wide.TIER_MANIFEST}) or reviewed in REVIEWED_SCANNERS",
        )


NATIVE_SOURCE = re.compile(
    r"^(core|apple|android|bindings|examples|external|inspect|packages|ship|test|"
    r"tools/(ci|cli|cmake|audio|agent-capabilities)|\.github/(workflows|actions))/"
    r"|(^|/)CMakeLists\.txt$|\.(c|cc|cpp|h|hpp|m|mm|swift|cmake)$"
)
# Recorded 2026-09-26 against the tree this change landed on.
BASELINE_SKIP_NATIVE = {8693, 8729, 8736, 8743, 8744, 8838, 8839, 8852, 8857, 8889}
# The pull requests the widening admitted when this was recorded. References
# added later can only shrink the set, never grow it, so the replay asserts a
# subset; WIDENED_FLOOR catches an instrument that silently admits nothing
# (or everything is refused because the reference search broke).
WIDENED_ELIGIBLE = {8668, 8669, 8670, 8675, 8682, 8759, 8770, 8774, 8859}
WIDENED_FLOOR = 5


class ReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        data = json.loads((REPO / wide.REPLAY_FIXTURE).read_text(encoding="utf-8"))
        cls.prs = data["prs"]
        cache: dict[str, list[str]] = {}
        cls.verdicts = {}
        for pr in cls.prs:
            files = pr["files"]
            base = (classify.native_build_required(files)
                    or classify.agent_capability_installed_sdk_required(files))
            widened = False
            if base and not classify.agent_capability_installed_sdk_required(files):
                kept = [f for f in files if not classify.is_skip_safe(f)]
                decisions = wide.classify(kept, repo=REPO, referrer_cache=cache,
                                          budget=64)
                widened = all(d.admitted for d in decisions.values())
            cls.verdicts[pr["number"]] = (base, widened)

    def test_fixture_is_the_recorded_window(self) -> None:
        self.assertEqual(len(self.prs), 177)

    def test_baseline_skip_native_set_is_unchanged(self) -> None:
        baseline = {n for n, (base, _) in self.verdicts.items() if not base}
        self.assertEqual(baseline, BASELINE_SKIP_NATIVE)

    def test_no_pull_request_with_native_sources_flips(self) -> None:
        flipped = [
            pr["number"] for pr in self.prs
            if self.verdicts[pr["number"]][1]
            and any(NATIVE_SOURCE.search(f) for f in pr["files"])
        ]
        self.assertEqual(flipped, [])

    def test_widened_set_is_bounded_and_nonempty(self) -> None:
        widened = {n for n, (_, w) in self.verdicts.items() if w}
        self.assertLessEqual(widened, WIDENED_ELIGIBLE)
        self.assertGreaterEqual(len(widened), WIDENED_FLOOR, sorted(widened))


if __name__ == "__main__":
    unittest.main(verbosity=1)
