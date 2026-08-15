#!/usr/bin/env python3
"""Regression contract for the centralized Shipyard merge steward."""

from pathlib import Path
import tomllib
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "shipyard-merge-steward.yml"
SHIPYARD_PIN = ROOT / "tools" / "shipyard.toml"


class ShipyardMergeStewardWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.doc = yaml.load(cls.text, Loader=yaml.BaseLoader)

    def test_controller_is_scheduled_and_serialized(self) -> None:
        triggers = self.doc["on"]
        self.assertEqual(set(triggers), {"schedule", "workflow_dispatch"})
        self.assertEqual(triggers["schedule"], [{"cron": "*/10 * * * *"}])
        concurrency = self.doc["concurrency"]
        self.assertIn("shipyard-merge-steward-", concurrency["group"])
        self.assertEqual(concurrency["cancel-in-progress"], "false")

    def test_schedule_applies_and_dispatches_recovery_while_manual_defaults_dry(self) -> None:
        env = self.doc["jobs"]["reconcile"]["env"]
        self.assertEqual(
            env["STEWARD_APPLY"],
            "${{ github.event_name == 'schedule' || inputs.apply }}",
        )
        self.assertEqual(
            env["STEWARD_DISPATCH_RECOVERY"],
            "${{ github.event_name == 'schedule' || inputs.dispatch_recovery }}",
        )
        self.assertIn("env.STEWARD_APPLY == 'true'", self.text)
        self.assertIn("env.STEWARD_DISPATCH_RECOVERY == 'true'", self.text)
        self.assertNotIn("if: inputs.apply", self.text)
        self.assertNotIn("if: always() && inputs.dispatch_recovery", self.text)

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
        self.assertIn(
            "actions/create-github-app-token@"
            "bcd2ba49218906704ab6c1aa796996da409d3eb1",
            self.text,
        )
        self.assertIn("secrets.SHIPYARD_APP_ID", self.text)
        self.assertIn("secrets.SHIPYARD_APP_PRIVATE_KEY", self.text)
        self.assertIn("permission-administration: read", self.text)
        self.assertIn("permission-contents: write", self.text)
        self.assertIn("permission-merge-queues: write", self.text)
        self.assertIn("steps.shipyard-app-token.outputs.token", self.text)
        self.assertNotIn("secrets.GITHUB_TOKEN", self.text)
        checkout = self.doc["jobs"]["reconcile"]["steps"][0]
        self.assertEqual(checkout["with"]["ref"], "main")
        self.assertEqual(checkout["with"]["persist-credentials"], "false")

    def test_exact_pinned_binary_and_machine_authority_are_proven(self) -> None:
        self.assertIn("./tools/install-shipyard.sh", self.text)
        self.assertIn('echo "$HOME/.local/bin" >> "$GITHUB_PATH"', self.text)
        self.assertIn("mutation_machine = \"github-actions\"", self.text)
        self.assertIn("shipyard runner tag --set github-actions", self.text)
        self.assertIn('status["authority_matches"] is True', self.text)

    def test_pinned_shipyard_preserves_steward_safety_floor(self) -> None:
        version = tomllib.loads(SHIPYARD_PIN.read_text(encoding="utf-8"))["shipyard"]["version"]
        parts = tuple(int(part) for part in version.removeprefix("v").split("."))
        self.assertGreaterEqual(
            parts,
            (0, 88, 0),
            "merge steward requires Shipyard v0.88.0 exact-head handoff semantics",
        )

    def test_evidence_ledger_is_restored_and_artifacts_are_bounded(self) -> None:
        steps = self.doc["jobs"]["reconcile"]["steps"]
        restore = next(step for step in steps
                       if step.get("name") == "Restore steward evidence ledger")
        save = next(step for step in steps
                    if step.get("name") == "Save steward evidence ledger")
        self.assertEqual(restore["uses"], "actions/cache/restore@v4")
        self.assertEqual(save["uses"], "actions/cache/save@v4")
        self.assertIn("always()", save["if"])
        self.assertIn("github.run_attempt", restore["with"]["key"])
        self.assertEqual(restore["with"]["key"], save["with"]["key"])
        self.assertIn("--ledger \"$STEWARD_LEDGER\"", self.text)
        self.assertIn(
            "name: shipyard-merge-steward-${{ github.run_id }}-"
            "${{ github.run_attempt }}",
            self.text,
        )
        self.assertIn("retention-days: 14", self.text)

    def test_pilot_disables_mutations_that_require_durable_remote_ledger(self) -> None:
        self.assertIn("--max-transient-reruns 0", self.text)
        self.assertIn("--no-coalesce", self.text)
        self.assertIn("--no-preempt-capacity", self.text)
        self.assertIn("Durable", self.text)
        self.assertIn("remote ledger storage is required", self.text)

    def test_apply_is_explicit_and_no_model_is_launched(self) -> None:
        workflow_dispatch = self.doc["on"]["workflow_dispatch"]
        self.assertEqual(workflow_dispatch["inputs"]["apply"]["default"], "false")
        self.assertEqual(
            workflow_dispatch["inputs"]["dispatch_recovery"]["default"], "false"
        )
        self.assertIn("args+=(--apply)", self.text)
        lowered = self.text.lower()
        for launcher in ("codex exec", "claude -p", "openai api"):
            self.assertNotIn(launcher, lowered)

    def test_exception_outbox_is_durable_and_health_failure_is_preserved(self) -> None:
        self.assertIn("permission-issues: write", self.text)
        steps = self.doc["jobs"]["reconcile"]["steps"]
        for name in ("Collect open PR facts", "Sync durable GitHub exception issue"):
            step = next(step for step in steps if step.get("name") == name)
            self.assertEqual(
                step["env"]["GH_TOKEN"],
                "${{ steps.shipyard-app-token.outputs.token }}",
            )
        self.assertIn("shipyard_steward_outbox.py", self.text)
        self.assertIn("shipyard:steward-outbox", self.text)
        self.assertIn("gh issue create", self.text)
        self.assertIn("gh issue reopen", self.text)
        self.assertIn("gh issue close", self.text)
        self.assertIn("Preserve outbox renderer failure", self.text)
        self.assertIn("steps.outbox.outcome != 'success'", self.text)
        self.assertIn("select(.title ==", self.text)
        self.assertIn("Preserve unhealthy controller result", self.text)
        self.assertIn('steps.reconcile.outputs.exit_code', self.text)

    def test_recovery_dispatch_is_exact_head_deduplicated_and_bounded(self) -> None:
        self.assertIn("shipyard_recovery_dispatch.py", self.text)
        self.assertIn("recovery-statuses.json", self.text)
        self.assertIn(".candidates[0] // empty", self.text)
        self.assertIn("shipyard/recovery-dispatch", self.text)
        self.assertIn("state=pending", self.text)
        self.assertIn("worker=pool", self.text)
        self.assertIn("recovery_url=$(gh workflow run", self.text)
        self.assertIn("actions/runs/${run_id}/cancel", self.text)
        self.assertIn("dispatch_failed attempt=${attempt}", self.text)
        self.assertIn("gh workflow run shipyard-recovery-worker.yml", self.text)
        self.assertIn("-f expected_head=\"$head\"", self.text)
        self.assertIn("-f assignment_epoch=\"$epoch\"", self.text)
        self.assertIn("-f blocker_fingerprint=\"$fingerprint\"", self.text)
        self.assertIn("-f publish_status=true", self.text)
        self.assertIn("-f attempt_repair=true", self.text)
        self.assertNotIn("actions/runners?per_page=100", self.text)
        self.assertNotIn("shipyard_recovery_worker_select.py", self.text)
        self.assertIn(
            "env.STEWARD_APPLY == 'true' && "
            "env.STEWARD_DISPATCH_RECOVERY == 'true'",
            self.text,
        )

    def test_stale_merge_group_cleanup_is_exact_head_revalidated_and_bounded(self) -> None:
        self.assertIn("shipyard_merge_queue_run_cleanup.py", self.text)
        self.assertIn("actions/runs?event=merge_group&status=${status}&per_page=100", self.text)
        self.assertIn("--limit 20", self.text)
        self.assertIn("merge-group-cleanup-plan.json", self.text)
        self.assertIn(".candidates[]", self.text)
        self.assertIn("head ${head} is current again", self.text)
        self.assertIn(".event == \"merge_group\"", self.text)
        self.assertIn(".head_sha == $head", self.text)
        self.assertIn("actions/runs/${run_id}/cancel", self.text)
        self.assertIn("env.STEWARD_APPLY == 'true' && steps.merge_group_cleanup_plan.outcome", self.text)
        self.assertIn("steps.merge_group_cleanup_apply.outcome", self.text)

    def test_superseded_pull_request_cleanup_is_exact_head_revalidated_and_bounded(self) -> None:
        self.assertIn("shipyard_pull_request_run_cleanup.py", self.text)
        self.assertIn("actions/runs?event=pull_request&status=${status}&per_page=100", self.text)
        self.assertIn("for status in in_progress pending queued requested waiting", self.text)
        self.assertIn("pulls?state=open&per_page=100", self.text)
        self.assertIn("--limit 20", self.text)
        self.assertIn("pull-request-cleanup-plan.json", self.text)
        self.assertIn("PR head ${head} is current again", self.text)
        self.assertIn('.event == "pull_request"', self.text)
        self.assertIn(".head_sha == $head", self.text)
        self.assertIn("actions/runs/${run_id}/cancel", self.text)
        self.assertIn("env.STEWARD_APPLY == 'true' && steps.pull_request_cleanup_plan.outcome", self.text)
        self.assertIn("steps.pull_request_cleanup_apply.outcome", self.text)
        self.assertNotIn('.event == "pull_request_target"', self.text)

    def test_unlabeled_prs_receive_only_a_truthful_exact_head_sentinel(self) -> None:
        self.assertIn("shipyard_provenance_label_plan.py", self.text)
        self.assertIn("provenance-label-plan.json", self.text)
        self.assertIn("shipyard:provenance-missing", self.text)
        self.assertIn('.state == "open" and .head.sha == $head', self.text)
        self.assertIn('if [ "$mutation" = add ]', self.text)
        self.assertIn('[ "${#labels[@]}" -eq 0 ] || continue', self.text)
        self.assertIn('elif [ "$mutation" = remove ]', self.text)
        self.assertIn('[ "${#labels[@]}" -gt 1 ] || continue', self.text)
        self.assertIn("env.STEWARD_APPLY == 'true' && steps.provenance_label_plan.outcome", self.text)
        self.assertIn("steps.provenance_label_apply.outcome", self.text)


if __name__ == "__main__":
    unittest.main()
