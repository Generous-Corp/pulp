#!/usr/bin/env python3
"""Regression contract for the bounded Subrouter-backed recovery worker."""

from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "shipyard-recovery-worker.yml"
CONFIG = ROOT / "tools" / "shipyard" / "recovery.config.toml"


class RecoveryWorkerWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.doc = yaml.load(cls.text, Loader=yaml.BaseLoader)
        cls.config = CONFIG.read_text(encoding="utf-8")

    def test_pilot_is_manual_unique_label_and_deduplicated(self) -> None:
        self.assertEqual(set(self.doc["on"]), {"workflow_dispatch"})
        job = self.doc["jobs"]["m5-luna-triage"]
        self.assertIn("shipyard-recovery-canary-m5-20260814", job["runs-on"])
        self.assertEqual(job["timeout-minutes"], "12")
        group = self.doc["concurrency"]["group"]
        for key in ("pr_number", "expected_head", "blocker_fingerprint"):
            self.assertIn(f"inputs.{key}", group)

    def test_status_mutation_is_explicitly_off_by_default(self) -> None:
        inputs = self.doc["on"]["workflow_dispatch"]["inputs"]
        self.assertEqual(inputs["publish_status"]["default"], "false")
        self.assertIn("always() && inputs.publish_status", self.text)

    def test_admin_secret_exists_only_in_lease_steps(self) -> None:
        steps = self.doc["jobs"]["m5-luna-triage"]["steps"]
        secret_steps = [
            step["name"]
            for step in steps
            if "SUBROUTER_SESSION_LEASE_ADMIN_TOKEN" in str(step)
        ]
        self.assertEqual(
            secret_steps,
            ["Acquire model-bound Subrouter lease", "Release Subrouter lease"],
        )
        self.assertIn("chmod 600", self.text)
        self.assertNotIn("session-lease.json\n", self.text.split("path: |", 1)[1])

    def test_exact_head_and_dispatch_are_revalidated_before_model(self) -> None:
        self.assertIn("shipyard_recovery_worker.py", self.text)
        self.assertIn("commits/${EXPECTED_HEAD}/statuses", self.text)
        self.assertIn("persist-credentials: false", self.text)
        self.assertIn("--strict-config exec", self.text)

    def test_profile_is_luna_low_ephemeral_read_only_and_lean(self) -> None:
        self.assertIn('model = "gpt-5.6-luna"', self.config)
        self.assertIn('model_reasoning_effort = "low"', self.config)
        self.assertIn('sandbox_mode = "read-only"', self.config)
        self.assertIn('env_key = "SUBROUTER_RECOVERY_TOKEN"', self.config)
        for disabled in ("plugins", "apps", "browser_use", "memories", "multi_agent", "goals"):
            self.assertIn(f"{disabled} = false", self.config)
        self.assertIn("--ephemeral", self.text)
        self.assertIn("--sandbox read-only", self.text)
        self.assertIn("--output-schema", self.text)

    def test_artifact_excludes_prompt_logs_and_lease_material(self) -> None:
        upload = next(
            step
            for step in self.doc["jobs"]["m5-luna-triage"]["steps"]
            if step.get("name") == "Publish sanitized triage artifact"
        )
        paths = upload["with"]["path"]
        self.assertIn("assignment.json", paths)
        self.assertIn("checks.json", paths)
        self.assertIn("recovery-result.json", paths)
        for forbidden in ("prompt.txt", "failed-checks.txt", "session-lease"):
            self.assertNotIn(forbidden, paths)


if __name__ == "__main__":
    unittest.main()
