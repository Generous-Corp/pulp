#!/usr/bin/env python3
"""Pin the CI-throughput workflow decisions and Linux example coverage."""

from __future__ import annotations

import json
import pathlib
import re
import subprocess
import sys
import unittest

try:
    import yaml
except ImportError:  # Required macOS CTest runners do not install PyYAML.
    yaml = None


ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "examples-validation.yml"
SANITIZERS = ROOT / ".github" / "workflows" / "sanitizers.yml"
TOPOLOGY = ROOT / "tools" / "scripts" / "runner_topology.json"
SHIPYARD_PROFILE = ROOT / ".shipyard" / "ci-profiles" / "normal-local-fast.toml"

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


class ShipyardTopologyContractTests(unittest.TestCase):
    def test_required_shipyard_macos_validation_keeps_examples_until_promotion(self) -> None:
        config = (ROOT / ".shipyard" / "config.toml").read_text(encoding="utf-8")
        default = toml_table(config, "validation.default")
        self.assertIn(
            "-DPULP_BUILD_EXAMPLES=ON",
            toml_json_value(default, "configure"),
        )

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


if __name__ == "__main__":
    unittest.main()
