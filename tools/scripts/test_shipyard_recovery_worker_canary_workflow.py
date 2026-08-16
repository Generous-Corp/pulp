#!/usr/bin/env python3
"""Regression contract for the disposable Shipyard recovery-worker canary."""

from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "shipyard-recovery-worker-canary.yml"


class RecoveryWorkerCanaryWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.doc = yaml.load(cls.text, Loader=yaml.BaseLoader)

    def test_is_manual_read_only_and_serialized_by_exact_assignment(self) -> None:
        self.assertEqual(set(self.doc["on"]), {"workflow_dispatch"})
        self.assertEqual(
            self.doc["permissions"],
            {"contents": "read", "pull-requests": "read"},
        )
        group = self.doc["concurrency"]["group"]
        self.assertIn("inputs.pr_number", group)
        self.assertIn("inputs.expected_head", group)
        self.assertEqual(self.doc["concurrency"]["cancel-in-progress"], "false")

    def test_unique_m5_disposable_label_is_not_a_generic_fleet_selector(self) -> None:
        labels = self.doc["jobs"]["m5-disposable-proof"]["runs-on"]
        self.assertEqual(
            labels,
            [
                "self-hosted",
                "macOS",
                "ARM64",
                "pulp-build-vm",
                "shipyard-recovery-canary-m5-20260814",
            ],
        )

    def test_exact_head_open_state_and_epoch_are_revalidated(self) -> None:
        self.assertIn('git rev-parse HEAD', self.text)
        self.assertIn('pulls/${PR_NUMBER}', self.text)
        self.assertIn('[ "$live_head" = "$EXPECTED_HEAD" ]', self.text)
        self.assertIn('[ "$live_state" = open ]', self.text)
        self.assertIn('ASSIGNMENT_EPOCH', self.text)
        self.assertIn('model_invocations:0', self.text)

    def test_canary_never_launches_a_model_or_persists_checkout_credentials(self) -> None:
        self.assertIn("persist-credentials: false", self.text)
        lowered = self.text.lower()
        for launcher in ("codex", "claude", "openai api", "anthropic"):
            self.assertNotIn(launcher, lowered)


if __name__ == "__main__":
    unittest.main()
