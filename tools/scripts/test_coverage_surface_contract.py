#!/usr/bin/env python3
"""Fail closed when a configured Codecov surface can silently disappear."""

from __future__ import annotations

import pathlib
import unittest
from typing import Any, Iterable

import yaml


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
MANIFEST = REPO_ROOT / "ci" / "coverage-surfaces.yaml"
CODECOV = REPO_ROOT / "codecov.yml"
CANONICAL_WORKFLOW = ".github/workflows/coverage.yml"
REACT_WORKFLOW = ".github/workflows/pulp-react-build.yml"
UPLOAD_ACTION = "./.github/actions/upload-codecov-report"
MATRIX_FLAG = "${{ matrix.flag }}"

EXPECTED_FLAGS = {
    "os-linux",
    "os-macos",
    "os-windows",
    "python-tools",
    "android-kotlin",
    "apple-swift",
    "pulp-react",
}
EXPECTED_NA = {"scene", "web", "rust", "sample-bank-manifest"}
REACT_PATHS = {
    "packages/pulp-react/**",
    "packages/pulp-import-ir/**",
    "test/fixtures/anchor_vectors.json",
    ".github/workflows/pulp-react-build.yml",
}
ALLOWED_EVENTS = {
    "pull-request",
    "pull-request-ios-paths",
    "pull-request-react-paths",
    "push-main",
    "schedule-8h",
    "workflow-dispatch",
}


def load_yaml(path: pathlib.Path, *, base: bool = False) -> dict[str, Any]:
    loader = yaml.BaseLoader if base else yaml.SafeLoader
    with path.open(encoding="utf-8") as handle:
        doc = yaml.load(handle, Loader=loader)
    if not isinstance(doc, dict):
        raise AssertionError(f"{path} must contain a YAML mapping")
    return doc


def scalar_values(value: Any) -> Iterable[str]:
    if isinstance(value, dict):
        for key, item in value.items():
            yield str(key)
            yield from scalar_values(item)
    elif isinstance(value, list):
        for item in value:
            yield from scalar_values(item)
    elif value is not None:
        yield str(value)


class CoverageSurfaceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = load_yaml(MANIFEST)
        cls.codecov = load_yaml(CODECOV)
        cls.workflows: dict[str, dict[str, Any]] = {}

    @classmethod
    def workflow(cls, relative_path: str) -> dict[str, Any]:
        if relative_path not in cls.workflows:
            cls.workflows[relative_path] = load_yaml(
                REPO_ROOT / relative_path, base=True
            )
        return cls.workflows[relative_path]

    def producers(self) -> list[dict[str, Any]]:
        return self.manifest["producers"]

    def test_policy_is_advisory_and_non_carryforward(self):
        self.assertEqual(self.manifest["schema_version"], 1)
        policy = self.manifest["policy"]
        self.assertEqual(policy["merge_behavior"], "advisory")
        self.assertEqual(policy["required_status_contexts"], [])
        self.assertIs(policy["carryforward"], False)
        self.assertEqual(policy["canonical_refresh_cron"], "17 */8 * * *")

    def test_exactly_seven_real_upload_flags_are_configured(self):
        producers = self.producers()
        ids = [producer["id"] for producer in producers]
        flags = [producer["flag"] for producer in producers]
        self.assertEqual(len(ids), len(set(ids)), "producer ids must be unique")
        self.assertEqual(len(flags), len(set(flags)), "producer flags must be unique")
        self.assertEqual(set(flags), EXPECTED_FLAGS)

        configured = self.codecov.get("flags", {})
        self.assertEqual(set(configured), EXPECTED_FLAGS)
        for flag, rules in configured.items():
            with self.subTest(flag=flag):
                self.assertEqual(rules, {"carryforward": False})

    def test_every_component_is_measured_or_explicitly_not_applicable(self):
        component_ids = {
            entry["component_id"]
            for entry in self.codecov["component_management"]["individual_components"]
        }
        component_sets = self.manifest["component_sets"]
        measured: set[str] = set()
        for producer in self.producers():
            set_names = producer.get("expected_component_sets", [])
            self.assertTrue(set_names, f"{producer['id']} has no expected components")
            for set_name in set_names:
                self.assertIn(set_name, component_sets)
                measured.update(component_sets[set_name])

        dispositions = self.manifest["not_applicable"]
        self.assertEqual({entry["id"] for entry in dispositions}, EXPECTED_NA)
        na_components = {
            entry["component"] for entry in dispositions if entry["component"] is not None
        }
        self.assertFalse(measured & na_components)
        self.assertEqual(
            component_ids,
            measured | na_components,
            "every configured component must have a producer expectation or an N/A disposition",
        )

        rust = next(entry for entry in dispositions if entry["id"] == "rust")
        self.assertIsNone(rust["component"])
        for entry in dispositions:
            self.assertTrue(entry["paths"])
            self.assertGreaterEqual(len(entry["reason"].split()), 8)

    def test_not_applicable_dispositions_match_the_tree(self):
        cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn(
            'option(PULP_ENABLE_SCENE3D "Enable opt-in native no-JS 3D scene/glTF renderer spikes" OFF)',
            cmake,
        )
        self.assertTrue((REPO_ROOT / "experimental/pulp-rs/Cargo.toml").is_file())
        self.assertTrue(list(REPO_ROOT.glob("core/**/platform/web/*")))

        sample_bank = REPO_ROOT / "core" / "sample_bank_manifest"
        instrumentable = {
            ".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".swift", ".py", ".kt",
            ".java", ".js", ".jsx", ".ts", ".tsx", ".rs",
        }
        self.assertFalse(
            [path for path in sample_bank.rglob("*") if path.suffix in instrumentable]
        )

        rust_coverage_commands: list[str] = []
        for path in (REPO_ROOT / ".github" / "workflows").glob("*.yml"):
            workflow = load_yaml(path, base=True)
            for job in workflow.get("jobs", {}).values():
                for step in job.get("steps", []):
                    command = step.get("run", "")
                    executable_lines = "\n".join(
                        line for line in command.splitlines()
                        if not line.lstrip().startswith("#")
                    )
                    if "cargo llvm-cov" in executable_lines or "cargo-llvm-cov" in executable_lines:
                        rust_coverage_commands.append(f"{path.name}: {executable_lines}")
        self.assertEqual(rust_coverage_commands, [])

    def test_producer_event_and_receipt_contract_is_complete(self):
        for producer in self.producers():
            with self.subTest(producer=producer["id"]):
                self.assertEqual(
                    producer.get("freshness_required", True),
                    producer["id"] != "native-windows",
                    "only the bounded best-effort Windows producer may be advisory",
                )
                self.assertTrue(producer["report"])
                self.assertTrue(producer["cadence"])
                self.assertTrue(producer["events"])
                self.assertLessEqual(set(producer["events"]), ALLOWED_EVENTS)
                self.assertTrue(producer["uploads"])
                for upload in producer["uploads"]:
                    self.assertIn(upload["flag_mode"], {"literal", "matrix"})
                    self.assertIn("${{ github.sha }}", upload["receipt"])
                    self.assertIn("${{ github.run_attempt }}", upload["receipt"])

    def test_manifest_uploads_exist_in_the_declared_jobs(self):
        for producer in self.producers():
            for upload in producer["uploads"]:
                case = f"{producer['id']}:{upload['workflow']}:{upload['job']}"
                with self.subTest(upload=case):
                    workflow = self.workflow(upload["workflow"])
                    jobs = workflow.get("jobs", {})
                    self.assertIn(upload["job"], jobs)
                    job = jobs[upload["job"]]
                    job_scalars = set(scalar_values(job))
                    self.assertIn(producer["report"], job_scalars)

                    action_steps = [
                        step
                        for step in job.get("steps", [])
                        if step.get("uses") == UPLOAD_ACTION
                    ]
                    self.assertTrue(action_steps, f"{case} has no Codecov upload action")

                    if upload["flag_mode"] == "matrix":
                        matches = [
                            step for step in action_steps
                            if step.get("with", {}).get("flags") == MATRIX_FLAG
                            and step.get("with", {}).get("receipt-name") == upload["receipt"]
                        ]
                        self.assertTrue(matches, f"{case} matrix upload contract drifted")
                        workflow_scalars = set(scalar_values(workflow))
                        self.assertTrue(
                            any(producer["flag"] in scalar for scalar in workflow_scalars),
                            f"{case} never materializes matrix flag {producer['flag']}",
                        )
                    else:
                        matches = [
                            step for step in action_steps
                            if step.get("with", {}).get("flags") == producer["flag"]
                            and step.get("with", {}).get("receipt-name") == upload["receipt"]
                            and producer["report"] in step.get("with", {}).get("files", "")
                        ]
                        self.assertTrue(matches, f"{case} literal upload contract drifted")

    def test_canonical_refresh_and_pr_triggers_cannot_disappear(self):
        canonical_on = self.workflow(CANONICAL_WORKFLOW)["on"]
        self.assertIn("pull_request", canonical_on)
        self.assertEqual(canonical_on["push"]["branches"], ["main"])
        self.assertEqual(
            canonical_on["schedule"],
            [{"cron": self.manifest["policy"]["canonical_refresh_cron"]}],
        )
        self.assertIn("workflow_dispatch", canonical_on)

        react_on = self.workflow(REACT_WORKFLOW)["on"]
        self.assertIn("pull_request", react_on)
        self.assertEqual(react_on["pull_request"]["branches"], ["main"])
        self.assertEqual(set(react_on["pull_request"]["paths"]), REACT_PATHS)


if __name__ == "__main__":
    unittest.main()
