#!/usr/bin/env python3
"""Pin windows-cli-compile.yml to the release build it stands in for.

The lane is only worth its runner if it compiles what release-cli.yml's Windows
legs compile, with the same options. These tests read both workflows and fail
when they drift, and they pin the names the release-time pre-tag check reads,
so a rename cannot silently turn that check into "no evidence, allow".

Run:  python3 tools/scripts/test_windows_cli_compile_workflow.py
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

import windows_cli_compile_scope as scope  # noqa: E402
import windows_cli_release_precheck as precheck  # noqa: E402

REPO = Path(__file__).resolve().parent.parent.parent
WORKFLOWS = REPO / ".github" / "workflows"


def load(name: str) -> dict:
    return yaml.safe_load((WORKFLOWS / name).read_text(encoding="utf-8"))


def triggers(workflow: dict) -> dict:
    # PyYAML reads the bare key `on` as boolean True.
    return workflow.get("on", workflow.get(True))


def step(job: dict, name: str) -> dict:
    for candidate in job["steps"]:
        if candidate.get("name") == name:
            return candidate
    raise AssertionError(f"step not found: {name}")


def define_flags(script: str) -> dict[str, str]:
    return dict(re.findall(r"-D([A-Z0-9_]+)=([^\s`\\]+)", script))


class CompileLane(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.lane = load("windows-cli-compile.yml")
        cls.release = load("release-cli.yml")
        cls.compile = cls.lane["jobs"]["compile"]
        cls.configure = step(
            cls.compile, "Configure (matches release-cli.yml Windows legs)"
        )["run"]

    def test_names_the_pretag_check_reads_exist(self) -> None:
        self.assertEqual(self.compile["name"], precheck.COMPILE_JOB)
        step(self.compile, precheck.COMPILE_STEP)
        self.assertEqual(
            precheck.WORKFLOW_FILE, "windows-cli-compile.yml",
            "the pre-tag check must read this workflow's runs",
        )

    def test_runs_on_the_release_image_and_never_a_paid_or_shared_pool(self) -> None:
        self.assertEqual(self.compile["runs-on"], "windows-latest")
        text = (WORKFLOWS / "windows-cli-compile.yml").read_text(encoding="utf-8")
        self.assertNotIn("NAMESPACE", text.upper().replace("NAMESPACELABS", ""))
        self.assertNotIn("self-hosted", text)

    def test_compile_runs_only_when_scope_says_so(self) -> None:
        self.assertEqual(self.compile["needs"], "scope")
        self.assertIn("needs.scope.outputs.relevant == 'true'", self.compile["if"])

    def test_configure_matches_release_literal_flags(self) -> None:
        release_configure = step(
            self.release["jobs"]["build-cli"], "Configure (CLI, no WebView on Linux)"
        )["run"]
        # Only the cmake invocation itself: the case arms above it assign the
        # per-platform flag variables, which test_configure_matches_release_
        # windows_branch checks separately.
        invocation = re.search(
            r"cmake -S \. -B build \\\n(?:[^\n]*\\\n)*[^\n]*", release_configure
        )
        self.assertIsNotNone(invocation, "release cmake invocation not found")
        literal = {
            key: value
            for key, value in define_flags(invocation.group(0)).items()
            if "$" not in value and not value.endswith(("'", '"'))
        }
        # Control: the parse must find the release's fixed flags, or every
        # comparison below passes vacuously.
        for expected in ("CMAKE_BUILD_TYPE", "PULP_SKIA_AUTOFETCH", "PULP_BUILD_TESTS",
                         "PULP_BUILD_EXAMPLES", "PULP_ENABLE_AUDIO_PROBES"):
            self.assertIn(expected, literal, "release configure parse is broken")
        ours = define_flags(self.configure)
        for key, value in literal.items():
            with self.subTest(flag=key):
                self.assertEqual(ours.get(key), value)

    def test_configure_matches_release_windows_branch(self) -> None:
        release_configure = step(
            self.release["jobs"]["build-cli"], "Configure (CLI, no WebView on Linux)"
        )["run"]
        # The GPU/Scene3D-on case list names the platforms that get them; the
        # Windows legs take the defaults. If Windows joins that list, this lane
        # must follow.
        case_list = re.search(r"\n\s*([a-z0-9|*-]+)\)\n\s*REQUIRE_GPU=", release_configure)
        self.assertIsNotNone(case_list, "release GPU case list not found")
        self.assertNotIn("windows", case_list.group(1))
        self.assertIn('REQUIRE_GPU="-DPULP_REQUIRE_GPU_FOR_SDK=OFF"', release_configure)
        self.assertIn('SCENE3D="-DPULP_ENABLE_SCENE3D=OFF"', release_configure)
        self.assertIn('INSPECTOR="-DPULP_ENABLE_INSPECTOR=ON"', release_configure)
        self.assertIn("'-DPULP_BUILD_WEBVIEW=OFF' || '-DPULP_BUILD_WEBVIEW=ON'",
                      release_configure)
        ours = define_flags(self.configure)
        self.assertEqual(ours.get("PULP_REQUIRE_GPU_FOR_SDK"), "OFF")
        self.assertEqual(ours.get("PULP_ENABLE_SCENE3D"), "OFF")
        self.assertEqual(ours.get("PULP_ENABLE_INSPECTOR"), "ON")
        self.assertEqual(ours.get("PULP_BUILD_WEBVIEW"), "ON")
        self.assertEqual(ours.get("CMAKE_CXX_COMPILER"), "cl")

    def test_builds_the_release_cli_targets(self) -> None:
        build = step(self.compile, precheck.COMPILE_STEP)["run"]
        self.assertIn("--target pulp-cli pulp-mcp", build)
        self.assertIn("-k 0", build)
        self.assertRegex(
            (REPO / "tools/cli/CMakeLists.txt").read_text(encoding="utf-8"),
            r"add_executable\(pulp-cli\b",
        )
        self.assertRegex(
            (REPO / "tools/mcp/CMakeLists.txt").read_text(encoding="utf-8"),
            r"add_executable\(pulp-mcp\b",
        )

    def test_push_paths_cover_the_always_relevant_trees(self) -> None:
        paths = triggers(self.lane)["push"]["paths"]
        for prefix in scope.ALWAYS_RELEVANT_PREFIXES:
            with self.subTest(prefix=prefix):
                self.assertIn(f"{prefix}**", paths)
        for own in scope.ALWAYS_RELEVANT_FILES:
            with self.subTest(file=own):
                self.assertIn(own, paths)
        self.assertNotIn("CMakeLists.txt", paths, "bump commits would start runs")

    def test_pull_request_trigger_is_unfiltered(self) -> None:
        # Path-filtering the trigger would make the check vanish on unrelated
        # PRs, which blocks forever once the check is required.
        self.assertIsNone(triggers(self.lane)["pull_request"])

    def test_main_pushes_never_cancel_each_other(self) -> None:
        concurrency = self.lane["concurrency"]
        self.assertIn("github.run_id", concurrency["group"])
        self.assertEqual(
            concurrency["cancel-in-progress"],
            "${{ github.event_name == 'pull_request' }}",
        )


class AutoReleaseWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = load("auto-release.yml")
        cls.tag = cls.workflow["jobs"]["tag"]

    def test_can_read_actions_but_not_cancel(self) -> None:
        self.assertEqual(self.workflow["permissions"].get("actions"), "read")

    def test_precheck_runs_before_tagging_and_gates_only_the_sdk(self) -> None:
        names = [s.get("name") for s in self.tag["steps"]]
        self.assertLess(
            names.index("Windows CLI pre-tag check"),
            names.index("Create tags for moved surfaces"),
        )
        check = step(self.tag, "Windows CLI pre-tag check")
        self.assertEqual(check["id"], "windows_precheck")
        self.assertIn("windows_cli_release_precheck.py", check["run"])
        env = step(self.tag, "Create tags for moved surfaces")["env"]
        self.assertIn("steps.windows_precheck.outputs.block == '1'", env["SDK_SHOULD_TAG"])
        self.assertNotIn("windows_precheck", env["PLUGIN_SHOULD_TAG"])

    def test_precheck_never_fails_the_job(self) -> None:
        run = step(self.tag, "Windows CLI pre-tag check")["run"]
        self.assertNotRegex(run, r"\bexit [1-9]")


if __name__ == "__main__":
    unittest.main()
