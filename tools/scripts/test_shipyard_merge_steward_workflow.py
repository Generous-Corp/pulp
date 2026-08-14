#!/usr/bin/env python3
"""Regression contract for the centralized Shipyard merge steward."""

from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "shipyard-merge-steward.yml"


class ShipyardMergeStewardWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.doc = yaml.load(cls.text, Loader=yaml.BaseLoader)

    def test_first_rollout_is_manual_only_and_serialized(self) -> None:
        triggers = self.doc["on"]
        self.assertEqual(set(triggers), {"workflow_dispatch"})
        concurrency = self.doc["concurrency"]
        self.assertIn("shipyard-merge-steward-", concurrency["group"])
        self.assertEqual(concurrency["cancel-in-progress"], "false")

    def test_controller_runs_off_fleet_with_minimum_mutation_permissions(self) -> None:
        job = self.doc["jobs"]["reconcile"]
        self.assertEqual(job["runs-on"], "ubuntu-latest")
        self.assertIn("PULP_PRIMARY_REPO", job["if"])
        self.assertIn("refs/heads/main", job["if"])
        permissions = self.doc["permissions"]
        self.assertEqual(permissions["contents"], "read")
        self.assertEqual(permissions["actions"], "write")
        for permission in ("statuses", "issues", "pull-requests"):
            self.assertNotIn(permission, permissions)

    def test_mutations_use_repository_scoped_shipyard_app_token(self) -> None:
        self.assertIn("actions/create-github-app-token@", self.text)
        self.assertIn("secrets.SHIPYARD_APP_ID", self.text)
        self.assertIn("secrets.SHIPYARD_APP_PRIVATE_KEY", self.text)
        self.assertIn("steps.shipyard-app-token.outputs.token", self.text)
        self.assertNotIn("secrets.GITHUB_TOKEN", self.text)
        checkout = self.doc["jobs"]["reconcile"]["steps"][0]
        self.assertEqual(checkout["with"]["ref"], "main")
        self.assertEqual(checkout["with"]["persist-credentials"], "false")

    def test_exact_pinned_binary_and_machine_authority_are_proven(self) -> None:
        self.assertIn("./tools/install-shipyard.sh", self.text)
        self.assertIn("mutation_machine = \"github-actions\"", self.text)
        self.assertIn("shipyard runner tag --set github-actions", self.text)
        self.assertIn('status["authority_matches"] is True', self.text)

    def test_retry_ledger_is_restored_and_evidence_is_bounded(self) -> None:
        self.assertIn("actions/cache@v4", self.text)
        self.assertIn("--ledger \"$STEWARD_LEDGER\"", self.text)
        self.assertIn("retention-days: 14", self.text)

    def test_apply_is_explicit_and_no_model_is_launched(self) -> None:
        workflow_dispatch = self.doc["on"]["workflow_dispatch"]
        self.assertEqual(workflow_dispatch["inputs"]["apply"]["default"], "false")
        self.assertIn("args+=(--apply)", self.text)
        lowered = self.text.lower()
        for launcher in ("codex exec", "claude -p", "openai api"):
            self.assertNotIn(launcher, lowered)


if __name__ == "__main__":
    unittest.main()
