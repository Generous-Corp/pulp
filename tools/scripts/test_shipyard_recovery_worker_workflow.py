#!/usr/bin/env python3
"""Regression contract for the bounded Subrouter-backed recovery worker."""

from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "shipyard-recovery-worker.yml"
CONFIG = ROOT / "tools" / "shipyard" / "recovery.config.toml"
REPAIR_CONFIG = ROOT / "tools" / "shipyard" / "repair.config.toml"


class RecoveryWorkerWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.doc = yaml.load(cls.text, Loader=yaml.BaseLoader)
        cls.config = CONFIG.read_text(encoding="utf-8")
        cls.repair_config = REPAIR_CONFIG.read_text(encoding="utf-8")

    def test_pilot_is_manual_unique_label_and_deduplicated(self) -> None:
        self.assertEqual(set(self.doc["on"]), {"workflow_dispatch"})
        job = self.doc["jobs"]["luna-triage"]
        self.assertIn("${{ format('shipyard-recovery-{0}', inputs.worker) }}", job["runs-on"])
        self.assertEqual(job["timeout-minutes"], "45")
        group = self.doc["concurrency"]["group"]
        for key in (
            "pr_number",
            "expected_head",
            "blocker_fingerprint",
            "assignment_epoch",
            "dispatch_attempt",
        ):
            self.assertIn(f"inputs.{key}", group)

    def test_status_mutation_is_explicitly_off_by_default(self) -> None:
        inputs = self.doc["on"]["workflow_dispatch"]["inputs"]
        self.assertIn("dispatch_attempt", inputs)
        self.assertIn("worker", inputs)
        self.assertIn("runner_name", inputs)
        self.assertEqual(inputs["publish_status"]["default"], "false")
        self.assertEqual(inputs["attempt_repair"]["default"], "false")
        self.assertIn("always() && inputs.publish_status", self.text)
        self.assertIn("context='shipyard/recovery-dispatch'", self.text)
        self.assertIn("completed attempt=${{ inputs.dispatch_attempt }}", self.text)
        self.assertIn("description=\"c=${classification}", self.text)
        self.assertEqual(self.text.count('[ "${#description}" -le 140 ]'), 1)
        self.assertEqual(self.text.count('[ "${#dispatch_description}" -le 140 ]'), 1)

    def test_selected_runner_name_is_fenced_before_checkout(self) -> None:
        steps = self.doc["jobs"]["luna-triage"]["steps"]
        self.assertEqual(steps[0]["name"], "Fence exact selected runner")
        self.assertIn('[ "$RUNNER_NAME" = "$EXPECTED_RUNNER_NAME" ]', steps[0]["run"])
        self.assertEqual(steps[1]["name"], "Install pinned Codex CLI in the disposable VM")
        self.assertEqual(steps[2]["name"], "Checkout trusted recovery harness")

    def test_disposable_vm_installs_integrity_pinned_codex(self) -> None:
        job = self.doc["jobs"]["luna-triage"]
        self.assertEqual(job["env"]["CODEX_RECOVERY_VERSION"], "0.147.0")
        self.assertTrue(job["env"]["CODEX_RECOVERY_PACKAGE_INTEGRITY"].startswith("sha512-"))
        self.assertTrue(
            job["env"]["CODEX_RECOVERY_DARWIN_ARM64_INTEGRITY"].startswith("sha512-")
        )
        install = job["steps"][1]["run"]
        self.assertIn("npm view", install)
        self.assertIn("--global --ignore-scripts --prefix", install)
        self.assertIn('"codex-cli ${CODEX_RECOVERY_VERSION}"', install)
        self.assertIn('echo "$install_root/bin" >> "$GITHUB_PATH"', install)

    def test_admin_secret_exists_only_in_lease_steps(self) -> None:
        steps = self.doc["jobs"]["luna-triage"]["steps"]
        secret_steps = [
            step["name"]
            for step in steps
            if "SUBROUTER_SESSION_LEASE_ADMIN_TOKEN" in str(step)
        ]
        self.assertEqual(
            secret_steps,
            [
                "Acquire model-bound Subrouter lease",
                "Release Subrouter lease",
                "Acquire Sol-medium Subrouter lease",
                "Release Sol-medium Subrouter lease",
            ],
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

    def test_sol_repair_is_one_opt_in_fenced_post_triage_attempt(self) -> None:
        self.assertIn('model = "gpt-5.6-sol"', self.repair_config)
        self.assertIn('model_reasoning_effort = "medium"', self.repair_config)
        self.assertIn('sandbox_mode = "workspace-write"', self.repair_config)
        self.assertIn("network_access = false", self.repair_config)
        self.assertIn("steps.triage.outputs.classification == 'needs_sol_fix'", self.text)
        self.assertEqual(self.text.count("Run one ephemeral Sol-medium repair"), 1)
        self.assertIn("--force-with-lease=\"refs/heads/${head_ref}:${EXPECTED_HEAD}\"", self.text)
        self.assertIn("Shipyard-Recovery-For", self.text)
        self.assertIn("shipyard/steward-handoff", self.text)

    def test_push_credential_is_minted_only_after_models_exit(self) -> None:
        model_end = self.text.index("Release Sol-medium Subrouter lease")
        token = self.text.index("Mint post-model Shipyard repair token")
        push = self.text.index("Fence, commit, and push repaired exact head")
        self.assertLess(model_end, token)
        self.assertLess(token, push)
        self.assertIn("permission-contents: write", self.text)
        publisher = self.doc["jobs"]["publish-recovery-result"]
        self.assertEqual(publisher["runs-on"], "ubuntu-latest")
        model_job = str(self.doc["jobs"]["luna-triage"])
        self.assertNotIn("SHIPYARD_APP_PRIVATE_KEY", model_job)
        self.assertNotIn("git push", model_job)

    def test_artifact_excludes_prompt_logs_and_lease_material(self) -> None:
        upload = next(
            step
            for step in self.doc["jobs"]["luna-triage"]["steps"]
            if step.get("name") == "Publish sanitized recovery artifact"
        )
        paths = upload["with"]["path"]
        self.assertIn("assignment.json", paths)
        self.assertIn("checks.json", paths)
        self.assertIn("recovery-result.json", paths)
        for forbidden in ("prompt.txt", "failed-checks.txt", "session-lease"):
            self.assertNotIn(forbidden, paths)


if __name__ == "__main__":
    unittest.main()
