#!/usr/bin/env python3
"""Tests for stale-running queue reconciliation lifecycle helpers."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("queue_reconcile_lifecycle.py", module_name="pulp_queue_reconcile_lifecycle")


def load_orchestrator_module():
    return load_local_ci_module(
        "queue_orchestrator.py",
        module_name="pulp_queue_orchestrator_for_reconcile_lifecycle_tests",
        add_module_dir=True,
    )


class QueueReconcileLifecycleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_reconcile_applies_one_stale_running_action_per_iteration(self) -> None:
        superseded_job = {"id": "old", "status": "running"}
        requeued_job = {"id": "retry", "status": "running"}
        replacement = {"id": "new"}
        queue = [superseded_job, requeued_job, replacement]
        events: list[tuple[str, object]] = []

        def actions(_queue, stale):
            events.append(("actions", [job["id"] for job in stale]))
            if stale == [superseded_job, requeued_job]:
                return [
                    {
                        "action": "supersede",
                        "job": superseded_job,
                        "replacement": replacement,
                        "reason": "newer_sha",
                    },
                    {"action": "requeue", "job": requeued_job},
                ]
            return [{"action": "requeue", "job": requeued_job}]

        result = self.mod.reconcile_running_jobs_unlocked(
            queue,
            stale_running_jobs_unlocked_fn=lambda loaded_queue: [superseded_job, requeued_job],
            stale_running_reconciliation_actions_unlocked_fn=actions,
            supersede_job_unlocked_fn=lambda job, replacement_id, reason: (
                events.append(("supersede", (job["id"], replacement_id, reason))),
                job.update(status="completed"),
            ),
            requeue_stale_running_job_unlocked_fn=lambda job: (
                events.append(("requeue", job["id"])),
                job.update(status="pending"),
            ),
        )

        self.assertEqual(result, (queue, True))
        self.assertEqual(
            events,
            [
                ("actions", ["old", "retry"]),
                ("supersede", ("old", "new", "newer_sha")),
                ("actions", ["retry"]),
                ("requeue", "retry"),
            ],
        )

    def test_reconcile_recomputes_actions_against_current_queue(self) -> None:
        orchestrator = load_orchestrator_module()
        older = {
            "id": "older",
            "branch": "feature/q",
            "sha": "a" * 40,
            "fingerprint": "older",
            "targets": ["mac"],
            "priority": "normal",
            "validation": "full",
            "queued_at": "2026-06-09T00:00:00+00:00",
            "status": "running",
        }
        newer = dict(
            older,
            id="newer",
            sha="b" * 40,
            fingerprint="newer",
            queued_at="2026-06-09T00:01:00+00:00",
        )
        queue = [older, newer]
        events: list[tuple[str, object]] = []

        result = self.mod.reconcile_running_jobs_unlocked(
            queue,
            stale_running_jobs_unlocked_fn=lambda loaded_queue: list(loaded_queue),
            stale_running_reconciliation_actions_unlocked_fn=orchestrator.stale_running_reconciliation_actions_unlocked,
            supersede_job_unlocked_fn=lambda job, replacement_id, reason: (
                events.append(("supersede", (job["id"], replacement_id, reason))),
                job.update(status="completed"),
            ),
            requeue_stale_running_job_unlocked_fn=lambda job: (
                events.append(("requeue", job["id"])),
                job.update(status="pending"),
            ),
        )

        self.assertEqual(result, (queue, True))
        self.assertEqual(events, [("supersede", ("older", "newer", "newer_sha_queued")), ("requeue", "newer")])


if __name__ == "__main__":
    unittest.main()
