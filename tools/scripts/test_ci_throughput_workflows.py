#!/usr/bin/env python3
"""Pin the CI-throughput workflow decisions and Linux example coverage."""

from __future__ import annotations

import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

try:
    import yaml
except ImportError:  # Required macOS CTest runners do not install PyYAML.
    yaml = None


ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "examples-validation.yml"
SANITIZERS = ROOT / ".github" / "workflows" / "sanitizers.yml"
ANDROID = ROOT / ".github" / "workflows" / "android.yml"
TOPOLOGY = ROOT / "tools" / "scripts" / "runner_topology.json"
SHIPYARD_PROFILE = ROOT / ".shipyard" / "ci-profiles" / "normal-local-fast.toml"
CORE_AUDIO_SERIAL_SUITES = {
    ROOT / "test" / "cmake" / "app_audio_host_tests.cmake": (
        "pulp-test-coreaudio-default-follow",
        "pulp-test-coreaudio-input-only",
    ),
    ROOT / "test" / "cmake" / "core_audio_platform_format_tests.cmake": (
        "pulp-test-audio",
    ),
    ROOT / "test" / "cmake" / "core_runtime_canvas_signal_tests.cmake": (
        "pulp-test-standalone-apply-config",
        "pulp-test-standalone-audio-inspector",
    ),
}
WEIGHTED_CORE_AUDIO_SERIAL_SUITES = {
    "pulp-test-audio",
    "pulp-test-standalone-apply-config",
    "pulp-test-standalone-audio-inspector",
}
NON_DEVICE_WEIGHTED_SUITES = (
    "pulp-test-standalone-editor-chrome",
    "pulp-test-standalone-audio-capture-wav",
    "pulp-test-standalone-audio-capture-rolling-wav",
    "pulp-test-standalone-transport-midi",
    "pulp-test-screenshot-capture",
)

def load_workflow(path: pathlib.Path) -> dict:
    if yaml is None:
        raise unittest.SkipTest("PyYAML unavailable; workflow-lint enforces YAML contracts")
    parsed = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(parsed, dict):
        raise TypeError(f"{path} did not parse as a mapping")
    return parsed


def trigger_map(workflow: dict) -> dict:
    # PyYAML 1.1 treats the unquoted key `on` as boolean true.
    return workflow.get("on", workflow.get(True, {}))


def step_script(job: dict) -> str:
    return "\n".join(str(step.get("run", "")) for step in job["steps"])


def named_step(job: dict, name: str) -> dict:
    matches = [step for step in job["steps"] if step.get("name") == name]
    if len(matches) != 1:
        raise AssertionError(f"expected exactly one {name!r} step")
    return matches[0]


def toml_table(text: str, header: str) -> str:
    match = re.search(
        rf"^\[{re.escape(header)}\]\n(.*?)(?=^\[|\Z)",
        text,
        re.M | re.S,
    )
    if match is None:
        raise AssertionError(f"missing TOML table [{header}]")
    return match.group(1)


def toml_json_value(table: str, key: str):
    match = re.search(rf"^{re.escape(key)}\s*=\s*(.+)$", table, re.M)
    if match is None:
        raise AssertionError(f"missing TOML key {key}")
    return json.loads(match.group(1))


def shipyard_ctest_contract_errors(command: str) -> list[str]:
    argv = shlex.split(command)
    errors = []
    if argv[:2] != ["tools/ci/governed-build.sh", "ctest"]:
        errors.append("CTest is not routed through the host-share governor")
    if any(
        # CTest also accepts signed positive levels (`-j+8`, `-j=+8`). Reject
        # the option families fail-closed instead of cloning its numeric grammar.
        arg.startswith("--parallel") or arg.startswith("-j")
        for arg in argv
    ):
        errors.append("CTest overrides the governor-provided parallelism")
    return errors


def pulp_test_suite_call(text: str, name: str) -> str:
    match = re.search(
        rf"pulp_add_test_suite\(\s*{re.escape(name)}\b.*?\)",
        text,
        re.S,
    )
    if match is None:
        raise AssertionError(f"missing pulp_add_test_suite registration for {name}")
    return match.group(0)


class ExamplesValidationWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = load_workflow(WORKFLOW)
        cls.jobs = cls.workflow["jobs"]

    def test_linux_job_builds_the_full_examples_tree(self) -> None:
        linux_job = self.jobs["build-linux"]
        self.assertIn("PULP_LOCAL_LINUX_RUNS_ON_JSON", linux_job["runs-on"])
        self.assertIn("github.event_name == 'workflow_dispatch'", linux_job["runs-on"])
        self.assertIn("ubuntu-latest", linux_job["runs-on"])
        self.assertTrue(
            any(
                step.get("uses") == "./.github/actions/install-linux-build-deps"
                for step in linux_job["steps"]
            )
        )
        self.assertIn(
            "fetch_skia_for_release.py linux-x64",
            named_step(linux_job, "Fetch Skia (linux-x64)")["run"],
        )
        self.assertIn(
            "-DPULP_BUILD_EXAMPLES=ON",
            named_step(linux_job, "Configure (examples ON)")["run"],
        )
        self.assertIn(
            "tools/ci/governed-build.sh cmake --build build --config Release "
            "--target pulp-examples-all",
            named_step(linux_job, "Build all examples")["run"],
        )

    def test_private_example_selectors_are_dispatch_only(self) -> None:
        resolver = next(
            step
            for step in self.jobs["resolve-advisory-macos"]["steps"]
            if step.get("id") == "resolve"
        )
        self.assertIn(
            "resolve_advisory_macos_runner.py",
            resolver["run"],
        )
        self.assertIn('--event-name "${{ github.event_name }}"', resolver["run"])

    def test_stable_report_requires_linux_and_macos(self) -> None:
        gate = self.jobs["gate"]
        self.assertEqual(gate["needs"], ["changes", "validate", "build-linux"])
        report = named_step(gate, "Report validation outcome")["run"]
        self.assertIn("needs.changes.result", report)
        self.assertIn("needs.changes.outputs.examples", report)
        self.assertIn("needs.build-linux.result", report)
        self.assertIn("needs.validate.result", report)

    def test_stable_report_decision_table(self) -> None:
        cases = (
            (("failure", "", "skipped", "skipped"), False),
            (("success", "false", "skipped", "skipped"), True),
            (("success", "true", "success", "success"), True),
            (("success", "true", "failure", "success"), False),
            (("success", "true", "success", "failure"), False),
            (("success", "true", "skipped", "skipped"), False),
        )
        for inputs, expected in cases:
            with self.subTest(inputs=inputs):
                script = named_step(
                    self.jobs["gate"], "Report validation outcome"
                )["run"]
                for expression, value in zip(
                    (
                        "${{ needs.changes.result }}",
                        "${{ needs.changes.outputs.examples }}",
                        "${{ needs.validate.result }}",
                        "${{ needs.build-linux.result }}",
                    ),
                    inputs,
                ):
                    script = script.replace(expression, value)
                result = subprocess.run(
                    ["bash", "-c", script],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(result.returncode == 0, expected, result.stdout)

    def test_change_detector_executes_the_canonical_classifier(self) -> None:
        changes = self.jobs["changes"]
        base_policy = named_step(changes, "Check out base-pinned example-path policy")
        self.assertEqual(base_policy["with"]["ref"],
                         "${{ github.event.pull_request.base.sha || github.sha }}")
        self.assertEqual(base_policy["with"]["path"], ".base-ci-control")
        self.assertIn(
            ".base-ci-control/tools/scripts/example_validation_paths.py",
            step_script(changes),
        )
        self.assertIn(
            'classifier="tools/scripts/example_validation_paths.py"',
            step_script(changes),
        )

class SanitizerCadenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = load_workflow(SANITIZERS)

    def test_automatic_matrix_is_pr_plus_nightly_not_post_merge(self) -> None:
        triggers = trigger_map(self.workflow)
        self.assertIn("pull_request", triggers)
        self.assertIn("schedule", triggers)
        self.assertNotIn("push", triggers)

    def test_nightly_entry_job_is_fork_guarded(self) -> None:
        condition = self.workflow["jobs"]["resolve-runners"]["if"]
        self.assertIn(
            "github.event_name != 'schedule' || "
            "github.repository == 'Generous-Corp/pulp'",
            condition,
        )

    def test_memory_sanitizers_skip_non_sanitizer_ios_builds(self) -> None:
        for job_name, display_name in (("asan", "ASan"), ("ubsan", "UBSan")):
            with self.subTest(job=job_name):
                script = named_step(
                    self.workflow["jobs"][job_name],
                    f"Test with {display_name}",
                )["run"]
                self.assertIn("cmake-ios-auv3-configure", script)
                self.assertIn("cmake-ios-hostapp-links", script)

    def test_asan_skips_optimized_fdn_stability_certification(self) -> None:
        script = named_step(
            self.workflow["jobs"]["asan"], "Test with ASan"
        )["run"]
        self.assertIn(
            "fdn reverb stays bounded and decaying for every parameter vector",
            script,
        )


class AndroidFixtureWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.job = load_workflow(ANDROID)["jobs"]["android-run-fixtures"]

    def test_emulator_uses_one_explicit_avd_home(self) -> None:
        step = named_step(self.job, "Create and boot x86_64 emulator")
        self.assertEqual(
            step["env"]["ANDROID_AVD_HOME"],
            "${{ runner.temp }}/android-avd",
        )
        script = step["run"]
        self.assertIn('-p "$ANDROID_AVD_HOME/pulp-fixtures.avd"', script)
        self.assertIn("emulator\" -list-avds | grep -Fx pulp-fixtures", script)

    def test_emulator_readiness_is_bounded_and_diagnostic(self) -> None:
        script = named_step(
            self.job, "Create and boot x86_64 emulator"
        )["run"]
        self.assertNotIn("adb wait-for-device", script)
        self.assertIn("adb get-state", script)
        self.assertIn("kill -0 \"$emulator_pid\"", script)
        self.assertIn("pulp-emulator.log", script)


class ShipyardTopologyContractTests(unittest.TestCase):
    def test_required_shipyard_macos_validation_keeps_examples_until_promotion(self) -> None:
        config = (ROOT / ".shipyard" / "config.toml").read_text(encoding="utf-8")
        default = toml_table(config, "validation.default")
        self.assertIn(
            "-DPULP_BUILD_EXAMPLES=ON",
            toml_json_value(default, "configure"),
        )

    def test_required_shipyard_ctest_uses_the_governed_host_share(self) -> None:
        config = (ROOT / ".shipyard" / "config.toml").read_text(encoding="utf-8")
        default = toml_table(config, "validation.default")
        command = toml_json_value(default, "test")

        self.assertEqual(shipyard_ctest_contract_errors(command), [])
        for preserved in (
            "--repeat until-pass:2",
            "--exclude-regex AudioWorkgroup",
            '--label-exclude "validation|slow|performance|bench|quality-lab"',
        ):
            self.assertIn(preserved, command)

    def test_parser_shipyard_ctest_uses_the_governed_host_share(self) -> None:
        config = (ROOT / ".shipyard" / "config.toml").read_text(encoding="utf-8")
        parser = toml_table(config, "validation.parser")
        command = toml_json_value(parser, "test")

        self.assertEqual(shipyard_ctest_contract_errors(command), [])
        for preserved in (
            "--label-include parser-import",
            "--exclude-regex AudioWorkgroup",
            "--label-exclude slow",
        ):
            self.assertIn(preserved, command)

    def test_shipyard_ctest_contract_accepts_governed_environment_only(self) -> None:
        for command in (
            "tools/ci/governed-build.sh ctest --test-dir build",
            "tools/ci/governed-build.sh ctest --test-dir build-jobs",
            "tools/ci/governed-build.sh ctest --test-dir build "
            '--label-exclude "slow|parallel"',
        ):
            with self.subTest(command=command):
                self.assertEqual(shipyard_ctest_contract_errors(command), [])

    def test_shipyard_ctest_contract_rejects_every_explicit_parallel_form(self) -> None:
        suffix = "--test-dir build --output-on-failure"
        for command in (
            f"ctest {suffix}",
            f"tools/ci/governed-build.sh ctest -j {suffix}",
            f"tools/ci/governed-build.sh ctest -j8 {suffix}",
            f"tools/ci/governed-build.sh ctest -j=8 {suffix}",
            f"tools/ci/governed-build.sh ctest -j+8 {suffix}",
            f"tools/ci/governed-build.sh ctest -j=+8 {suffix}",
            f"tools/ci/governed-build.sh ctest --parallel {suffix}",
            f"tools/ci/governed-build.sh ctest --parallel 8 {suffix}",
            f"tools/ci/governed-build.sh ctest --parallel=8 {suffix}",
            f"tools/ci/governed-build.sh ctest --parallel=+8 {suffix}",
        ):
            with self.subTest(command=command):
                self.assertTrue(shipyard_ctest_contract_errors(command))

    def test_shipyard_required_macos_targets_match_topology_contract(self) -> None:
        profile = SHIPYARD_PROFILE.read_text(encoding="utf-8")
        topology = json.loads(TOPOLOGY.read_text(encoding="utf-8"))
        contracted = next(
            lane["expect"]
            for lane in topology["lanes"]
            if lane["variable"] == "PULP_LOCAL_MACOS_RUNS_ON_JSON"
        )
        route_text = toml_table(profile, 'repo."Generous-Corp/pulp".pr.macos')
        self.assertIn(
            'github_variable = "PULP_LOCAL_MACOS_RUNS_ON_JSON"',
            route_text,
        )
        targets = toml_json_value(route_text, "targets")
        declared_selectors = {
            json.dumps(lane["expect"], sort_keys=True)
            for lane in topology["lanes"]
        }
        declared_selectors.update(
            json.dumps(label)
            for label in topology["github_hosted_labels"]
        )
        selectors = []
        for target in targets:
            with self.subTest(target=target):
                target_block = toml_table(profile, f'targets."{target}"')
                selector = toml_json_value(target_block, "runs_on_json")
                selectors.append(selector)
                self.assertIn(
                    json.dumps(selector, sort_keys=True),
                    declared_selectors,
                    "fallback target must use a selector declared in topology",
                )
        self.assertEqual(selectors[0], contracted)


class CTestIsolationContractTests(unittest.TestCase):
    def test_every_processors_eight_registration_is_classified(self) -> None:
        expected = (
            WEIGHTED_CORE_AUDIO_SERIAL_SUITES
            | set(NON_DEVICE_WEIGHTED_SUITES)
            | {"agent-capability-manifest-selftest"}
        )
        actual = set()
        for path in (ROOT / "test" / "cmake").glob("*.cmake"):
            text = path.read_text(encoding="utf-8")
            for match in re.finditer(
                r"pulp_add_test_suite\(\s*([^\s)]+).*?\)", text, re.S
            ):
                if re.search(r"\bPROCESSORS\s+8\b", match.group(0)):
                    actual.add(match.group(1))
            for match in re.finditer(
                r"set_tests_properties\(\s*([^\s)]+).*?\)", text, re.S
            ):
                if re.search(r"\bPROCESSORS\s+8\b", match.group(0)):
                    actual.add(match.group(1))
        self.assertEqual(actual, expected)

    def test_real_coreaudio_suites_are_capacity_independently_serial(self) -> None:
        for path, names in CORE_AUDIO_SERIAL_SUITES.items():
            text = path.read_text(encoding="utf-8")
            for name in names:
                with self.subTest(path=path.name, suite=name):
                    call = pulp_test_suite_call(text, name)
                    self.assertRegex(call, r"\bRUN_SERIAL\s+TRUE\b")
                    if name in WEIGHTED_CORE_AUDIO_SERIAL_SUITES:
                        self.assertRegex(call, r"\bPROCESSORS\s+8\b")
                    else:
                        self.assertNotRegex(call, r"\bPROCESSORS\b")

    def test_non_device_weighted_suites_are_not_serialized(self) -> None:
        path = ROOT / "test" / "cmake" / "core_runtime_canvas_signal_tests.cmake"
        text = path.read_text(encoding="utf-8")
        for name in NON_DEVICE_WEIGHTED_SUITES:
            with self.subTest(suite=name):
                call = pulp_test_suite_call(text, name)
                self.assertRegex(call, r"\bPROCESSORS\s+8\b")
                self.assertNotRegex(call, r"\bRUN_SERIAL\b")

    def test_unrelated_quality_weight_is_not_globally_serialized(self) -> None:
        quality = (ROOT / "test" / "cmake" / "quality_tests.cmake").read_text(
            encoding="utf-8"
        )
        registration = re.search(
            r"set_tests_properties\(agent-capability-manifest-selftest\s+"
            r"PROPERTIES\s+PROCESSORS\s+8\)",
            quality,
        )
        self.assertIsNotNone(registration)
        self.assertNotIn("RUN_SERIAL", registration.group(0))

    @staticmethod
    def _run_serial_scheduler_fixture(
        run_serial: bool,
    ) -> tuple[subprocess.CompletedProcess, bool]:
        with tempfile.TemporaryDirectory() as raw_tmp:
            tmp = pathlib.Path(raw_tmp)
            marker = tmp / "isolated-running"
            overlap = tmp / "witness-overlapped"
            worker = tmp / "worker.py"
            worker.write_text(
                """#!/usr/bin/env python3
import pathlib
import sys
import time

mode, marker_arg, overlap_arg = sys.argv[1:]
marker = pathlib.Path(marker_arg)
overlap = pathlib.Path(overlap_arg)
if mode == "isolated":
    marker.write_text("running", encoding="utf-8")
    time.sleep(1.0)
    marker.unlink(missing_ok=True)
else:
    deadline = time.monotonic() + 1.5
    while time.monotonic() < deadline:
        if marker.exists():
            overlap.write_text("overlap", encoding="utf-8")
            break
        time.sleep(0.01)
""",
                encoding="utf-8",
            )
            q = json.dumps
            properties = 'PROCESSORS "8" COST "1000"'
            if run_serial:
                properties += ' RUN_SERIAL "TRUE"'
            lines = [
                f"add_test(isolated {q(sys.executable)} {q(str(worker))} isolated "
                f"{q(str(marker))} {q(str(overlap))})",
                f"set_tests_properties(isolated PROPERTIES {properties})",
            ]
            for index in range(4):
                lines.append(
                    f"add_test(witness-{index} {q(sys.executable)} {q(str(worker))} "
                    f"witness {q(str(marker))} {q(str(overlap))})"
                )
            (tmp / "CTestTestfile.cmake").write_text(
                "\n".join(lines) + "\n", encoding="utf-8"
            )
            result = subprocess.run(
                ["ctest", "--test-dir", str(tmp), "--output-on-failure"],
                capture_output=True,
                text=True,
                check=False,
                env={**os.environ, "CTEST_PARALLEL_LEVEL": "12"},
            )
            return result, overlap.exists()

    def test_run_serial_prevents_overlap_above_the_historical_width(self) -> None:
        governed, governed_overlap = self._run_serial_scheduler_fixture(True)
        self.assertEqual(governed.returncode, 0, governed.stdout + governed.stderr)
        self.assertFalse(governed_overlap, governed.stdout + governed.stderr)

        control, control_overlap = self._run_serial_scheduler_fixture(False)
        self.assertEqual(control.returncode, 0, control.stdout + control.stderr)
        self.assertTrue(
            control_overlap,
            "negative control did not reproduce overlap without RUN_SERIAL\n"
            + control.stdout
            + control.stderr,
        )


if __name__ == "__main__":
    unittest.main()
